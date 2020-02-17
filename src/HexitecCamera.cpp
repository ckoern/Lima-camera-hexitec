//###########################################################################
// This file is part of LImA, a Library for Image Acquisition
//
// Copyright (C) : 2009-2016
// European Synchrotron Radiation Facility
// BP 220, Grenoble 38043
// FRANCE
//
// This is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// This software is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//###########################################################################

#include <iostream>
#include <chrono>
#include <unistd.h>
#include <cfloat>
#include <future>
#include <atomic>

#include <HexitecApi.h>

#include "lima/Debug.h"
#include "lima/Constants.h"
#include "lima/Exceptions.h"
#include "lima/CtBuffer.h"
#include "processlib/PoolThreadMgr.h"
#include "processlib/TaskMgr.h"
#include "processlib/TaskEventCallback.h"
#include "HexitecCamera.h"


using namespace lima;
using namespace lima::Hexitec;
using namespace std;

typedef std::chrono::high_resolution_clock Clock;

class Camera::TaskEventCb: public TaskEventCallback {
DEB_CLASS_NAMESPC(DebModCamera, "Camera", "EventCb");
public:
	TaskEventCb(Camera& cam);
	virtual ~TaskEventCb();
	void finished(Data& d);
private:
	Camera& m_cam;
};

//-----------------------------------------------------
// AcqThread class
//-----------------------------------------------------
class Camera::AcqThread: public Thread {
DEB_CLASS_NAMESPC(DebModCamera, "Camera", "AcqThread");
public:
	AcqThread(Camera &aCam);
	virtual ~AcqThread();

protected:
	virtual void threadFunction();

private:
	TaskEventCb* m_eventCb;
	Data m_lastFrame;
	Camera& m_cam;
};

//-----------------------------------------------------
// TimerThread class
//-----------------------------------------------------
class Camera::TimerThread: public Thread {
DEB_CLASS_NAMESPC(DebModCamera, "Camera", "TimerThread");
public:
	TimerThread(Camera &aCam);
	virtual ~TimerThread();

protected:
	virtual void threadFunction();

private:
	Camera& m_cam;
};

//-----------------------------------------------------
// internal private structure
//-----------------------------------------------------
struct Camera::Private
{
	std::unique_ptr<Camera::AcqThread> m_acq_thread;
	std::unique_ptr<Camera::TimerThread> m_timer_thread;
	std::unique_ptr<HexitecAPI::HexitecApi> m_hexitec;
	std::atomic<bool> m_quit;
	std::atomic<bool> m_acq_started;
	std::atomic<bool> m_thread_running;
	std::atomic<bool> m_finished_saving;
	std::atomic<int> m_image_number;
	std::atomic<int> m_status;
	std::future<void> m_future_result;
};


//-----------------------------------------------------
// @brief camera constructor
//-----------------------------------------------------
Camera::Camera(const std::string& ipAddress, const std::string& configFilename, int bufferCount, int timeout, int asicPitch) :
		m_ipAddress(ipAddress), m_configFilename(configFilename), m_bufferCount(bufferCount), m_timeout(timeout),
		m_asicPitch(asicPitch), m_trig_mode(IntTrig),
		m_detectorImageType(Bpp16), m_detector_type("Hexitec"), m_detector_model("V1.0.0"), m_maxImageWidth(80),
		m_maxImageHeight(80), m_x_pixelsize(1), m_y_pixelsize(1), m_offset_x(0), m_offset_y(0),
		m_collectDcTimeout(10000), m_processType(ProcessType::CSA),
		m_saveOpt(Camera::SaveRaw), m_binWidth(10), m_speclen(8000), m_lowThreshold(0), m_highThreshold(10000),
		m_biasVoltageRefreshInterval(10000), m_biasVoltageRefreshTime(5000), m_biasVoltageSettleTime(2000) {

	DEB_CONSTRUCTOR();



	m_private = std::shared_ptr<Private>(new Private);
	m_private->m_acq_started = false;
	m_private->m_quit = false;
	m_framesPerTrigger = 0;

	m_bufferCtrlObj = new SoftBufferCtrlObj();


	setStatus(Camera::Initialising);
	m_private->m_hexitec = std::unique_ptr < HexitecAPI::HexitecApi > (new HexitecAPI::HexitecApi(ipAddress, m_timeout));
	initialise();

	// Acquisition Thread
	m_private->m_acq_thread = std::unique_ptr < AcqThread > (new AcqThread(*this));
	m_private->m_acq_started = false;
	m_private->m_acq_thread->start();

	// Timer thread for cycling the bias voltage
	m_private->m_timer_thread = std::unique_ptr < TimerThread > (new TimerThread(*this));
	m_private->m_timer_thread->start();

	setStatus(Camera::Ready);
	DEB_TRACE() << "Camera constructor complete";
}

//-----------------------------------------------------
// @brief camera destructor
//-----------------------------------------------------
Camera::~Camera() {
	DEB_DESTRUCTOR();
	setHvBiasOff();
	m_private->m_hexitec->closePipeline();
	m_private->m_hexitec->closeStream();
	PoolThreadMgr::get().quit();
	delete m_bufferCtrlObj;
}

//----------------------------------------------------------------------------
// initialize detector
//----------------------------------------------------------------------------
void Camera::initialise() {
	DEB_MEMBER_FUNCT();
	int32_t rc;
	uint32_t errorCode;
	std::string errorCodeString;
	std::string errorDescription;
	if (m_private->m_hexitec->readConfiguration(m_configFilename) != HexitecAPI::NO_ERROR) {
		THROW_HW_ERROR(Error) << "Failed to read the configuration file " << DEB_VAR1(m_configFilename);
	}

	m_private->m_hexitec->initDevice(errorCode, errorCodeString, errorDescription);
	if (errorCode != HexitecAPI::NO_ERROR) {
		DEB_TRACE() << "Error      :" << errorCodeString;
		DEB_TRACE() << "Description:" << errorDescription;
		THROW_HW_ERROR(Error) << errorDescription << " " << DEB_VAR1(errorCode);
	}
	DEB_TRACE() << "Error code :" << errorCode;

	uint8_t useTermChar = true;
	rc = m_private->m_hexitec->openSerialPortBulk0((2 << 16), useTermChar, 0x0d);
	if (rc != HexitecAPI::NO_ERROR) {
		THROW_HW_ERROR(Error) << "Failed to open serial port " << DEB_VAR1(rc);
	}
	uint8_t customerId;
	uint8_t projectId;
	uint8_t version;
	uint8_t forceEqualVersion = false;
	rc = m_private->m_hexitec->checkFirmware(customerId, projectId, version, forceEqualVersion);
	if (rc != HexitecAPI::NO_ERROR) {
		THROW_HW_ERROR(Error) << "Failed to read firmware version information " << DEB_VAR1(rc);
	}
	DEB_TRACE() << "customerId :" << int(customerId);
	DEB_TRACE() << "projectId  :" << int(projectId);
	DEB_TRACE() << "version    :" << int(version);

	uint8_t width;
	uint8_t height;
	uint32_t collectDcTime;

	rc = m_private->m_hexitec->configureDetector(width, height, m_frameTime, collectDcTime);
	if (rc != HexitecAPI::NO_ERROR) {
		THROW_HW_ERROR(Error) << "Failed to configure the detector " << DEB_VAR1(rc);
	}
	DEB_TRACE() << "width         :" << int(width);
	DEB_TRACE() << "height        :" << int(height);
	DEB_TRACE() << "frameTime     :" << m_frameTime;
	DEB_TRACE() << "collectDcTime :" << collectDcTime;

	double humidity;
	double ambientTemperature;
	double asicTemperature;
	double adcTemperature;
	double ntcTemperature;
	rc = m_private->m_hexitec->readEnvironmentValues(humidity, ambientTemperature, asicTemperature, adcTemperature, ntcTemperature);
	if (rc != HexitecAPI::NO_ERROR) {
		THROW_HW_ERROR(Error) << "Failed to read environmental values " << DEB_VAR1(rc);
	}
	DEB_TRACE() << "humidity           :" << humidity;
	DEB_TRACE() << "ambientTemperature :" << ambientTemperature;
	DEB_TRACE() << "asicTemperature    :" << asicTemperature;
	DEB_TRACE() << "adcTemperature     :" << adcTemperature;
	DEB_TRACE() << "ntcTemperature     :" << ntcTemperature;
	double v3_3;
	double hvMon;
	double hvOut;
	double v1_2;
	double v1_8;
	double v3;
	double v2_5;
	double v3_3ln;
	double v1_65ln;
	double v1_8ana;
	double v3_8ana;
	double peltierCurrent;

	rc = m_private->m_hexitec->readOperatingValues(v3_3, hvMon, hvOut, v1_2, v1_8, v3, v2_5, v3_3ln, v1_65ln, v1_8ana, v3_8ana, peltierCurrent,
			ntcTemperature);
	if (rc != HexitecAPI::NO_ERROR) {
		THROW_HW_ERROR(Error) << "Failed to read operating values" << DEB_VAR1(rc);
	}
	DEB_TRACE() << "v3_3           :" << v3_3;
	DEB_TRACE() << "hvMon          :" << hvMon;
	DEB_TRACE() << "hvOut          :" << hvOut;
	DEB_TRACE() << "v1_2           :" << v1_2;
	DEB_TRACE() << "v1_8           :" << v1_8;
	DEB_TRACE() << "v3             :" << v3;
	DEB_TRACE() << "v2_5           :" << v2_5;
	DEB_TRACE() << "v3_3ln         :" << v3_3ln;
	DEB_TRACE() << "v1_65ln        :" << v1_65ln;
	DEB_TRACE() << "v1_8ana        :" << v1_8ana;
	DEB_TRACE() << "v3_8ana        :" << v3_8ana;
	DEB_TRACE() << "peltierCurrent :" << peltierCurrent;
	DEB_TRACE() << "ntcTemperature :" << ntcTemperature;

	rc = m_private->m_hexitec->setFrameFormatControl("Mono16", m_maxImageWidth, m_maxImageHeight, m_offset_x, m_offset_y, "One", "Off"); //IPEngineTestPattern");
	if (rc != HexitecAPI::NO_ERROR) {
		THROW_HW_ERROR(Error) << "Failed to set frame format control " << DEB_VAR1(rc);
	}

	//openStream needs to be called before createPipeline
	rc = m_private->m_hexitec->openStream();
	if (rc != HexitecAPI::NO_ERROR) {
		THROW_HW_ERROR(Error) << "Failed to open stream" << DEB_VAR1(rc);
	}
	rc = m_private->m_hexitec->closePipeline();
	DEB_TRACE() << "setting buffer count to " << m_bufferCount;
	rc = m_private->m_hexitec->createPipelineOnly(m_bufferCount);
	if (rc != HexitecAPI::NO_ERROR) {
		THROW_HW_ERROR(Error) << "Failed to create pipeline" << DEB_VAR1(rc);
	}
}

//-----------------------------------------------------------------------------
// @brief Prepare the detector for acquisition
//-----------------------------------------------------------------------------
void Camera::prepareAcq() {
	DEB_MEMBER_FUNCT();
	m_private->m_image_number = 0;
	setHvBiasOn();
	// wait asynchronously for the HV Bias to settle
	// check the result before start acquisition in Acq thread.
	m_private->m_future_result = std::async(std::launch::async, [=] {std::chrono::milliseconds(m_biasVoltageRefreshTime);});

	Size image_size;
    ImageType image_type;

    getDetectorMaxImageSize(image_size);
    getImageType(image_type);

    FrameDim frame_dim(image_size, image_type);
    m_bufferCtrlObj->setFrameDim(frame_dim);
    m_bufferCtrlObj->setNbBuffers(m_bufferCount);

    if (m_framesPerTrigger == 0)
        m_framesPerTrigger = m_nb_frames;
    if (m_trig_mode == ExtTrigSingle || m_trig_mode == ExtTrigMult) {
        DEB_ALWAYS() << "Number of frames per trigger " << m_framesPerTrigger;
        m_private->m_hexitec->setTriggeredFrameCount(m_framesPerTrigger);
    }
}

//-----------------------------------------------------------------------------
// @brief start the acquisition
//-----------------------------------------------------------------------------
void Camera::startAcq() {
	DEB_MEMBER_FUNCT();
	AutoMutex lock(m_cond.mutex());
	m_errCount = 0;
	m_private->m_acq_started = true;
	m_saved_frame_nb = 0;
	m_cond.broadcast();
}

//-----------------------------------------------------------------------------
// @brief stop the acquisition
//-----------------------------------------------------------------------------
void Camera::stopAcq() {
	DEB_MEMBER_FUNCT();
	AutoMutex lock(m_cond.mutex());
	if (m_private->m_acq_started)
		m_private->m_acq_started = false;
}

//-----------------------------------------------------------------------------
// @brief return the detector Max image size
//-----------------------------------------------------------------------------
void Camera::getDetectorMaxImageSize(Size& size) {
	DEB_MEMBER_FUNCT();
	size = Size(m_maxImageWidth, m_maxImageHeight);
}

//-----------------------------------------------------------------------------
// @brief return the detector image size
//-----------------------------------------------------------------------------
void Camera::getDetectorImageSize(Size& size) {
	DEB_MEMBER_FUNCT();
	getDetectorMaxImageSize(size);
}

//-----------------------------------------------------------------------------
// @brief Get the image type
//-----------------------------------------------------------------------------
void Camera::getImageType(ImageType& type) {
	DEB_MEMBER_FUNCT();
	type = m_detectorImageType;
}

//-----------------------------------------------------------------------------
// @brief set Image type
//-----------------------------------------------------------------------------
void Camera::setImageType(ImageType type) {
	DEB_MEMBER_FUNCT();
	if (type != Bpp16)
		THROW_HW_ERROR(NotSupported) << DEB_VAR1(type) << " Only Bpp16 supported";
}

//-----------------------------------------------------------------------------
// @brief return the detector type
//-----------------------------------------------------------------------------
void Camera::getDetectorType(string& type) {
	DEB_MEMBER_FUNCT();
	type = m_detector_type;
}

//-----------------------------------------------------------------------------
// @brief return the detector model
//-----------------------------------------------------------------------------
void Camera::getDetectorModel(string& model) {
	DEB_MEMBER_FUNCT();
	model = m_detector_model;
}

//-----------------------------------------------------------------------------
// @brief Checks trigger mode
//-----------------------------------------------------------------------------
bool Camera::checkTrigMode(TrigMode trig_mode) {
	DEB_MEMBER_FUNCT();
	DEB_PARAM() << DEB_VAR1(trig_mode);
	switch (trig_mode) {
	case IntTrig:
	case ExtTrigSingle:
    case ExtTrigMult:
	case ExtGate:
		return true;
	case IntTrigMult:
	default:
		return false;
	}
}

//-----------------------------------------------------------------------------
// @brief Set the new trigger mode
//-----------------------------------------------------------------------------
void Camera::setTrigMode(TrigMode trig_mode) {
	DEB_MEMBER_FUNCT();
    DEB_ALWAYS() << "Setting trigger mode:" << DEB_VAR1(trig_mode);
    m_private->m_hexitec->setTriggerCountingMode((trig_mode != IntTrig));
    m_private->m_hexitec->disableTriggerGate();
    m_private->m_hexitec->disableTriggerMode();
    switch (trig_mode) {
	case IntTrig:
	    break;
	case ExtTrigSingle:
	case ExtTrigMult:
	    m_private->m_hexitec->enableTriggerMode();
	    break;
	case ExtGate:
	    m_private->m_hexitec->enableTriggerGate();
		break;
	case IntTrigMult:
	default:
		THROW_HW_ERROR(NotSupported) << DEB_VAR1(trig_mode);
	}
	m_trig_mode = trig_mode;
}

//-----------------------------------------------------------------------------
// @brief Get the current trigger mode
//-----------------------------------------------------------------------------
void Camera::getTrigMode(TrigMode& mode) {
	DEB_MEMBER_FUNCT();
	mode = m_trig_mode;
	DEB_RETURN() << DEB_VAR1(mode);
}

//-----------------------------------------------------------------------------
/// @brief Set the new exposure time
//-----------------------------------------------------------------------------
void Camera::setExpTime(double exp_time) {
	DEB_MEMBER_FUNCT();
	DEB_PARAM() << DEB_VAR1(exp_time);

	DEB_TRACE() << "setExpTime " << DEB_VAR3(m_exp_time, m_nb_frames, m_frameTime);
	m_exp_time = exp_time;
	if (m_nb_frames == 0) {
		m_nb_frames = int(m_exp_time / m_frameTime);
	}
	DEB_TRACE() << "setExpTime " << DEB_VAR3(m_exp_time, m_nb_frames, m_frameTime);
}

//-----------------------------------------------------------------------------
// @brief Get the current exposure time
//-----------------------------------------------------------------------------
void Camera::getExpTime(double& exp_time) {
	DEB_MEMBER_FUNCT();
	exp_time = m_exp_time;
	DEB_RETURN() << DEB_VAR1(exp_time);
}

//-----------------------------------------------------------------------------
// @brief Set the new latency time between images
//-----------------------------------------------------------------------------
void Camera::setLatTime(double lat_time) {
	DEB_MEMBER_FUNCT();
	DEB_PARAM() << DEB_VAR1(lat_time);
	m_latency_time = lat_time;
}

//-----------------------------------------------------------------------------
// @brief Get the current latency time
//-----------------------------------------------------------------------------
void Camera::getLatTime(double& lat_time) {
	DEB_MEMBER_FUNCT();
	lat_time = m_latency_time;
	DEB_RETURN() << DEB_VAR1(lat_time);
}

//-----------------------------------------------------------------------------
// @brief Get the exposure time range
//-----------------------------------------------------------------------------
void Camera::getExposureTimeRange(double& min_expo, double& max_expo) const {
	DEB_MEMBER_FUNCT();
	// --- no info on min/max exposure
	min_expo = 0.0;
	max_expo = DBL_MAX;
}

//-----------------------------------------------------------------------------
// @brief Get the latency time range
//-----------------------------------------------------------------------------
void Camera::getLatTimeRange(double& min_lat, double& max_lat) const {
	DEB_MEMBER_FUNCT();
	// --- no info on min/max latency
	min_lat = 0.0;
	max_lat = DBL_MAX;
}

//-----------------------------------------------------------------------------
// @brief Set the number of frames to be taken
//-----------------------------------------------------------------------------
void Camera::setNbFrames(int nb_frames) {
	DEB_MEMBER_FUNCT();
	DEB_PARAM() << DEB_VAR1(nb_frames);
	m_nb_frames = nb_frames;
	m_exp_time = m_frameTime * m_nb_frames;
	DEB_TRACE() << "setNbFrames " << DEB_VAR3(m_exp_time, m_nb_frames, m_frameTime);
}

//-----------------------------------------------------------------------------
// @brief Get the number of frames to be taken
//-----------------------------------------------------------------------------
void Camera::getNbFrames(int& nb_frames) {
	DEB_MEMBER_FUNCT();
	nb_frames = m_nb_frames;
	DEB_RETURN() << DEB_VAR1(nb_frames);
}

//-----------------------------------------------------------------------------
// @brief Get the number of acquired frames
//-----------------------------------------------------------------------------
void Camera::getNbHwAcquiredFrames(int &nb_acq_frames) {
	DEB_MEMBER_FUNCT();
	nb_acq_frames = m_private->m_image_number;
}

//-----------------------------------------------------------------------------
// @brief Get the camera status
//-----------------------------------------------------------------------------
Camera::Status Camera::getStatus() {
	DEB_MEMBER_FUNCT();
	int status = m_private->m_status;
	return static_cast<Camera::Status>(status);
}

void Camera::setStatus(Camera::Status status) {
	AutoMutex lock(m_cond.mutex());
	m_private->m_status = static_cast<int>(status);
}

//-----------------------------------------------------------------------------
// @brief check if binning is available
//-----------------------------------------------------------------------------
bool Camera::isBinningAvailable() {
	DEB_MEMBER_FUNCT();
	return false;
}

//-----------------------------------------------------------------------------
// @brief return the detector pixel size
//-----------------------------------------------------------------------------
void Camera::getPixelSize(double& sizex, double& sizey) {
	DEB_MEMBER_FUNCT();
	sizex = m_x_pixelsize;
	sizey = m_y_pixelsize;
	DEB_RETURN() << DEB_VAR2(sizex, sizey);
}

//-----------------------------------------------------------------------------
// @brief reset the camera
//-----------------------------------------------------------------------------
void Camera::reset() {
	DEB_MEMBER_FUNCT();
	return;
}

HwBufferCtrlObj* Camera::getBufferCtrlObj() {
	return m_bufferCtrlObj;
}



//-----------------------------------------------------
// acquisition thread
//-----------------------------------------------------
Camera::AcqThread::AcqThread(Camera &cam) : m_cam(cam) {
	m_eventCb = new TaskEventCb(cam);
	pthread_attr_setscope(&m_thread_attr, PTHREAD_SCOPE_PROCESS);
}

Camera::AcqThread::~AcqThread() {
	DEB_DESTRUCTOR();
	AutoMutex lock(m_cam.m_cond.mutex());
	m_cam.m_private->m_quit = true;
	m_cam.m_cond.broadcast();
	lock.unlock();
	delete m_eventCb;
	DEB_TRACE()  << "Waiting for the acquisition thread to be done (joining the main thread).";
	join();
}

void Camera::AcqThread::threadFunction() {
	DEB_MEMBER_FUNCT();
	int32_t rc;
	uint16_t* bptr;
	auto trigger_failed = false;
	StdBufferCbMgr& buffer_mgr = m_cam.m_bufferCtrlObj->getBuffer();
	buffer_mgr.setStartTimestamp(Timestamp::now());
	while (true) {
		while (!m_cam.m_private->m_acq_started && !m_cam.m_private->m_quit) {
			DEB_TRACE() << "AcqThread Waiting ";
			m_cam.m_private->m_thread_running = false;
			AutoMutex lock(m_cam.m_cond.mutex());
			m_cam.m_cond.wait();
		}
		auto t1 = Clock::now();
		if (m_cam.m_private->m_quit) {
			return;
		}

		m_cam.m_private->m_finished_saving = false;
		m_cam.m_private->m_thread_running = true;
		m_cam.setStatus(Camera::Exposure);

		bool continue_acq = true;
		try {
			m_cam.m_private->m_future_result.get();
			DEB_ALWAYS() << "Starting acquisition";
			rc = m_cam.m_private->m_hexitec->startAcq();
			if (rc != HexitecAPI::NO_ERROR) {
				DEB_ERROR() << "Failed to start acquisition " << DEB_VAR1(rc);
				m_cam.setHvBiasOff();
				bool continue_acq = false;
			}
		} catch (Exception& e) {
			DEB_ERROR() << "Failed to start acquisition " << DEB_VAR1(rc);
			bool continue_acq = false;
		}
		int nbf;
		m_cam.m_bufferCtrlObj->getNbBuffers(nbf);
		DEB_TRACE() << DEB_VAR1(nbf);


	    while (continue_acq && m_cam.m_private->m_acq_started && (!m_cam.m_nb_frames || m_cam.m_private->m_image_number < m_cam.m_nb_frames)) {

			bptr = (uint16_t*) buffer_mgr.getFrameBufferPtr(m_cam.m_private->m_image_number);
			rc = m_cam.m_private->m_hexitec->retrieveBuffer((uint8_t*)bptr, m_cam.m_timeout);
			if (rc == HexitecAPI::NO_ERROR) {
				if (m_cam.getStatus() == Camera::Exposure) {
					DEB_TRACE() << "Image# " << m_cam.m_private->m_image_number << " acquired";
					HwFrameInfoType frame_info;
					frame_info.acq_frame_nb = m_cam.m_private->m_image_number;
					continue_acq = buffer_mgr.newFrameReady(frame_info);
					m_cam.m_private->m_image_number++;
				} else {
					std::this_thread::sleep_for(std::chrono::milliseconds(500));
				}
			} else if (rc == 27 || rc == 2818) {
			    m_cam.m_errCount++;
				DEB_WARNING() << "Skipping frame " << m_cam.m_private->m_hexitec->getErrorDescription() << " " << DEB_VAR1(rc);
            } else if (rc == 30 && m_cam.m_trig_mode == ExtGate && m_cam.m_private->m_image_number == 0) {
                DEB_ERROR() << "External Trigger probably failed " << m_cam.m_private->m_hexitec->getErrorDescription() << " " << DEB_VAR1(rc);
                trigger_failed = true;
                break;
            } else if (rc == 30 && m_cam.m_trig_mode == ExtGate && m_cam.m_private->m_image_number > 0) {
                rc = HexitecAPI::NO_ERROR; // just finished the gate & timed out
                break;
			} else {
				DEB_ERROR() << "Retrieve error " << m_cam.m_private->m_hexitec->getErrorDescription() << " " << DEB_VAR1(rc);
				break;
			}
		}
		DEB_TRACE() << m_cam.m_private->m_image_number << " images acquired";
		auto t2 = Clock::now();
		DEB_TRACE() << "Delta t2-t1: " << std::chrono::duration_cast < std::chrono::nanoseconds
				> (t2 - t1).count() << " nanoseconds";

		m_cam.m_private->m_acq_started = false;
		DEB_ALWAYS() << "Stop acquisition";
		auto rc2 = m_cam.m_private->m_hexitec->stopAcq();
		if (rc2 != HexitecAPI::NO_ERROR) {
		    DEB_ERROR() << "Failed to stop acquisition " << DEB_VAR1(rc);
		}
		if (!trigger_failed) {
            m_cam.setStatus(Camera::Readout);
            DEB_TRACE() << "Setting bias off";
            m_cam.setHvBiasOff();
            DEB_ALWAYS() << "Check for outstanding processes";
            
		}

		DEB_ALWAYS() << "Set status to ready";
		DEB_ALWAYS() << "Skipped frames " << m_cam.m_errCount;
		if (rc == HexitecAPI::NO_ERROR && rc2 == HexitecAPI::NO_ERROR) {
			m_cam.setStatus(Camera::Ready);
		} else {
			m_cam.setStatus(Camera::Fault);
		}
	}
}

//-----------------------------------------------------
// timer thread
//-----------------------------------------------------
Camera::TimerThread::TimerThread(Camera& cam) :
		m_cam(cam) {
	pthread_attr_setscope(&m_thread_attr, PTHREAD_SCOPE_PROCESS);
}

Camera::TimerThread::~TimerThread() {
	DEB_DESTRUCTOR();
	AutoMutex lock(m_cam.m_cond.mutex());
	m_cam.m_private->m_quit = true;
	m_cam.m_cond.broadcast();
	lock.unlock();
	DEB_TRACE()  << "Waiting for the timer thread to be done (joining the main thread)";
	join();
}

void Camera::TimerThread::threadFunction() {
	DEB_MEMBER_FUNCT();

	while (!m_cam.m_private->m_quit) {
		while (!m_cam.m_private->m_acq_started && !m_cam.m_private->m_quit) {
			DEB_TRACE() << "Timer thread waiting";
			AutoMutex lock(m_cam.m_cond.mutex());
			m_cam.m_cond.wait();
		}
		DEB_TRACE() << "Timer thread Running";
		if (m_cam.m_private->m_quit)
			return;

		std::this_thread::sleep_for(std::chrono::milliseconds(m_cam.m_biasVoltageRefreshInterval));
		if (m_cam.m_private->m_acq_started) {
			m_cam.setStatus(Camera::Paused);
			DEB_TRACE() << "Paused at frame " << DEB_VAR1(m_cam.m_private->m_image_number);
			m_cam.setHvBiasOff();
		if (m_cam.m_private->m_acq_started)
			std::this_thread::sleep_for(std::chrono::milliseconds(m_cam.m_biasVoltageRefreshTime));
        if (m_cam.m_private->m_acq_started)
			m_cam.setHvBiasOn();
			std::this_thread::sleep_for(std::chrono::milliseconds(m_cam.m_biasVoltageSettleTime));
	    if (m_cam.m_private->m_acq_started)
			m_cam.setStatus(Camera::Exposure);
			DEB_TRACE() << "Acq status in timer after restart " << DEB_VAR1(m_cam.m_private->m_status);
		} else {
            m_cam.setHvBiasOff();
		}
	}
}

//-----------------------------------------------------
// task event callback
//-----------------------------------------------------
Camera::TaskEventCb::TaskEventCb(Camera& cam) : m_cam(cam) {}

Camera::TaskEventCb::~TaskEventCb() {}

void Camera::TaskEventCb::finished(Data& data) {
    DEB_MEMBER_FUNCT();
	AutoMutex lock(m_cam.m_cond.mutex());
	m_cam.m_private->m_finished_saving = true;
}

//-----------------------------------------------------
// Hexitec specific stuff
//-----------------------------------------------------

//-----------------------------------------------------------------------------
// @brief get environmental values
//-----------------------------------------------------------------------------
void Camera::getEnvironmentalValues(Environment& env) {
	DEB_MEMBER_FUNCT();
	auto rc = m_private->m_hexitec->readEnvironmentValues(env.humidity, env.ambientTemperature, env.asicTemperature, env.adcTemperature,
			env.ntcTemperature);
	if (rc != HexitecAPI::NO_ERROR) {
		THROW_HW_ERROR(Error) << "Failed to read environmental values " << DEB_VAR1(rc);
	}
}

//-----------------------------------------------------------------------------
// @brief get operating values
//-----------------------------------------------------------------------------
void Camera::getOperatingValues(OperatingValues& opval) {
	DEB_MEMBER_FUNCT();
	auto rc = m_private->m_hexitec->readOperatingValues(opval.v3_3, opval.hvMon, opval.hvOut, opval.v1_2, opval.v1_8, opval.v3, opval.v2_5,
			opval.v3_3ln, opval.v1_65ln, opval.v1_8ana, opval.v3_8ana, opval.peltierCurrent, opval.ntcTemperature);
	if (rc != HexitecAPI::NO_ERROR) {
		THROW_HW_ERROR(Error) << "Failed to read operating values " << DEB_VAR1(rc);
	}
}

/**
 * Set dark current collection timeout
 * @param[in] timeout time in milliseconds
 */
void Camera::setCollectDcTimeout(int timeout) {
	m_collectDcTimeout = timeout;
}

void Camera::getCollectDcTimeout(int& timeout) {
	timeout = m_collectDcTimeout;
}

void Camera::getFrameTimeout(int& timeout) {
    timeout = m_timeout;
}
void Camera::setFrameTimeout(int timeout) {
    m_timeout = timeout;
}

void Camera::collectOffsetValues() {
	DEB_MEMBER_FUNCT();
    setHvBiasOn();
	auto rc = m_private->m_hexitec->collectOffsetValues(m_collectDcTimeout);
	if (rc != HexitecAPI::NO_ERROR) {
	    setHvBiasOff();
		THROW_HW_ERROR(Error) << "Failed to collect offset values! " << DEB_VAR1(rc);
	}
    setHvBiasOff();
}

void Camera::setType(ProcessType type) {
	m_processType = type;
}

void Camera::getType(ProcessType& type) {
	type = m_processType;
}

void Camera::setBinWidth(int binWidth) {
	m_binWidth = binWidth;
}

void Camera::getBinWidth(int& binWidth) {
	binWidth = m_binWidth;
}

void Camera::setSpecLen(int speclen) {
	m_speclen = speclen;
}

void Camera::getSpecLen(int& speclen) {
	speclen = m_speclen;
}

void Camera::setLowThreshold(int threshold) {
	m_lowThreshold = threshold;
}

void Camera::getLowThreshold(int& threshold) {
	threshold = m_lowThreshold;
}

void Camera::setHighThreshold(int threshold) {
	m_highThreshold = threshold;
}

void Camera::getHighThreshold(int& threshold) {
	threshold = m_highThreshold;
}

void Camera::setHvBiasOn() {
	DEB_MEMBER_FUNCT();
	auto rc = m_private->m_hexitec->setHvBiasOn(true);
	if (rc != HexitecAPI::NO_ERROR) {
		THROW_HW_ERROR(Error) << "Failed to set HV Bias on " << DEB_VAR1(rc);
	}
}
void Camera::setHvBiasOff() {
	DEB_MEMBER_FUNCT();
	auto rc = m_private->m_hexitec->setHvBiasOn(false);
	if (rc != HexitecAPI::NO_ERROR) {
		THROW_HW_ERROR(Error) << "Failed to turn HV Bias off" << DEB_VAR1(rc);
	}
    DEB_ALWAYS() << "HV Bias is now off";
}

void Camera::getFrameRate(double& rate) {
	rate = 0.001 / m_frameTime;
}

void Camera::setSaveOpt(int saveOpt) {
	m_saveOpt = saveOpt;
}

void Camera::getSaveOpt(int& saveOpt) {
	saveOpt = m_saveOpt;
}

void Camera::setBiasVoltageRefreshInterval(int millis) {
	m_biasVoltageRefreshInterval = millis;
}

void Camera::setBiasVoltageRefreshTime(int millis) {
	m_biasVoltageRefreshTime = millis;
}

void Camera::setBiasVoltageSettleTime(int millis) {
	m_biasVoltageSettleTime = millis;
}
void Camera::getBiasVoltageRefreshInterval(int& millis) {
	millis = m_biasVoltageRefreshInterval;
}

void Camera::getBiasVoltageRefreshTime(int& millis) {
    millis = m_biasVoltageRefreshTime;
}

void Camera::getBiasVoltageSettleTime(int& millis) {
	millis = m_biasVoltageSettleTime;
}

void Camera::setBiasVoltage(int volts) {
    m_private->m_hexitec->setBiasVoltage(volts);
}
void Camera::getBiasVoltage(int& volts) {
    m_private->m_hexitec->getBiasVoltage(volts);
}

void Camera::setRefreshVoltage(int volts) {
    m_private->m_hexitec->setRefreshVoltage(volts);
}

void Camera::getRefreshVoltage(int& volts) {
    m_private->m_hexitec->getRefreshVoltage(volts);
}

void Camera::setFramesPerTrigger(int nframes) {
    m_framesPerTrigger = nframes;
}

void Camera::getFramesPerTrigger(int& nframes) {
    nframes = m_framesPerTrigger;
}

void Camera::getSkippedFrameCount(int& count) {
    count = m_errCount;
}
