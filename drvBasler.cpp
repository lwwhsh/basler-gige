/*
 * Headers
 */
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <arpa/inet.h>

#include <epicsExport.h>
#include <drvSup.h>
#include <errlog.h>
#include <iocsh.h>

#include <pylon/PylonIncludes.h>
#include <pylon/gige/BaslerGigECamera.h>
#include <pylon/gige/BaslerGigECamera.h>

#include "drvBasler.h"

using namespace std;
using namespace Basler_GigECameraParams;
using namespace Pylon;

/*
 * Macros
 */
#define NUMBER_OF_DEVICES	3

typedef enum
{
	OPCODE_GET_IMAGE			,
	OPCODE_SET_GAIN_AUTO		,
	OPCODE_GET_GAIN_AUTO		,
	OPCODE_SET_GAIN				,
	OPCODE_GET_GAIN				,
	OPCODE_SET_EXPOSURE			,
	OPCODE_GET_EXPOSURE			,
	OPCODE_SET_WIDTH			,
	OPCODE_GET_WIDTH			,
	OPCODE_SET_HEIGHT			,
	OPCODE_GET_HEIGHT			,
	OPCODE_GET_SIZE				,
	OPCODE_SET_TRIGGER_SOURCE	,
	OPCODE_GET_TRIGGER_SOURCE	,
	OPCODE_SET_OFFSET_X			,
	OPCODE_GET_OFFSET_X			,
	OPCODE_SET_OFFSET_Y			,
	OPCODE_GET_OFFSET_Y
} opcode_t;

typedef struct
{
	char				name	[30];
	char				ip		[30];
	pthread_mutex_t		hardwareMutex;	
	pthread_mutex_t		syncMutex;
	pthread_cond_t  	conditionSignal;
	CBaslerGigECamera*	camera;
	CBaslerGigECamera::StreamGrabber_t* 	streamGrabber;
	opcode_t			opcode;
	uint8_t*			buffer;
	bool				gainAuto;
	uint32_t			gain;
	uint32_t			exposure;
	uint32_t			width;
	uint32_t			height;
	uint32_t			size;
	triggerSource_t		triggerSource;
	uint32_t			offsetX;
	uint32_t			offsetY;
} configuration_t;

/*
 * Private members
 */
static	configuration_t			configurations[NUMBER_OF_DEVICES];
static	int						deviceCount		=	0;
static	PylonAutoInitTerm 		autoInitTerm;

/*
 * Private function prototypes
 */
static	long	init(void);
static	long	report(int detail);
static	void* 	thread(void* arg);
static	void	getImage(configuration_t* configuration);
static	void	setGainAuto(CBaslerGigECamera* camera, bool gainAuto);
static	void	getGainAuto(CBaslerGigECamera* camera, bool *gainAuto);
static	void	setGain(CBaslerGigECamera* camera, uint32_t gain);
static	void	getGain(CBaslerGigECamera* camera, uint32_t *gain);
static	void	setExposure(CBaslerGigECamera* camera, uint32_t exposure);
static	void	getExposure(CBaslerGigECamera* camera, uint32_t *exposure);
static	void	setWidth(CBaslerGigECamera* camera, uint32_t width);
static	void	getWidth(CBaslerGigECamera* camera, uint32_t *width);
static	void	setHeight(CBaslerGigECamera* camera, uint32_t height);
static	void	getHeight(CBaslerGigECamera* camera, uint32_t *height);
static	void	getSize(CBaslerGigECamera* camera, uint32_t *size);
static	void	setTriggerSource(CBaslerGigECamera* camera, triggerSource_t source);
static	void	getTriggerSource(CBaslerGigECamera* camera, triggerSource_t* source);
static	void	setOffsetX(CBaslerGigECamera* camera, uint32_t offsetX);
static	void	getOffsetX(CBaslerGigECamera* camera, uint32_t *offsetX);
static	void	setOffsetY(CBaslerGigECamera* camera, uint32_t offsetY);
static	void	getOffsetY(CBaslerGigECamera* camera, uint32_t *offsetY);

/*
 * Function definitions
 */

static long 
init(void)
{
	int			status;
	pthread_t	handle;
	basler_t	device;

	for (device = 0; device < deviceCount; device++)
	{
		/*Initialize sync methods*/
		pthread_mutex_init(&configurations[device].hardwareMutex, NULL);
		pthread_mutex_init(&configurations[device].syncMutex, NULL);
		pthread_cond_init(&configurations[device].conditionSignal, NULL);

		/*Lock synchronization mutex*/
		pthread_mutex_lock(&configurations[device].syncMutex);

		/*Start new driver thread*/
		printf("Initializing %s @ %s\r\n", configurations[device].name, configurations[device].ip);
		status	=	pthread_create(&handle, NULL, thread, &configurations[device]);	
		if (status)
		{
			errlogPrintf("\x1B[31mUnable to initialize driver thread: Unable to create thread\r\n\x1B[0m");
			pthread_mutex_unlock(&configurations[device].syncMutex);
			return -1;
		}

		/*Wait for driver thread to initialize*/
		pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);
		pthread_mutex_unlock(&configurations[device].syncMutex);
	}

	return 0;
}

basler_t
basler_open(char *deviceName)
{
	basler_t device;

	for (device = 0; device < deviceCount; device++)
	{
		if (strcmp(configurations[device].name, deviceName) == 0)
			return (basler_t)device;
	}

    errlogPrintf("\x1B[31mUnable to open device: device %s not found\r\n\x1B[0m", deviceName);
	return -1;
}

void*
thread(void* arg)
{
	configuration_t*	configuration;
	IPylonDevice*		device;

	configuration			=	(configuration_t*)arg;
	device					=	CTlFactory::GetInstance().CreateDevice(CBaslerGigEDeviceInfo().SetIpAddress(configuration->ip));
	configuration->camera	=	new CBaslerGigECamera(device);
	configuration->camera->Open();

	configuration->camera->PixelFormat.SetValue(PixelFormat_Mono8);
	configuration->camera->GevSCPSPacketSize.SetValue(1500);
	configuration->camera->GevSCPD.SetValue(0);
	configuration->camera->GevSCFTD.SetValue(0);

	configuration->camera->AcquisitionMode.SetValue(AcquisitionMode_SingleFrame);

	configuration->camera->TriggerSelector.SetValue(TriggerSelector_AcquisitionStart);
	configuration->camera->TriggerMode.SetValue(TriggerMode_Off);
	configuration->camera->TriggerSelector.SetValue(TriggerSelector_FrameStart);
	configuration->camera->TriggerMode.SetValue(TriggerMode_On);
	configuration->camera->TriggerSource.SetValue(TriggerSource_Software);
	configuration->triggerSource	=	TRIGGER_SOURCE_SOFTWARE;
	configuration->camera->TriggerDelayAbs.SetValue(0);

	configuration->camera->GainAuto.SetValue(GainAuto_Continuous);
	configuration->gainAuto	=	true;

	configuration->streamGrabber	=	new CBaslerGigECamera::StreamGrabber_t(configuration->camera->GetStreamGrabber(0));
	configuration->streamGrabber->Open();

	/*Inform init() that driver thread has initialized*/
	pthread_mutex_lock(&configuration->syncMutex);
	pthread_cond_signal(&configuration->conditionSignal);

	while (true) 
	{ 
		/*Wait for command*/
		pthread_cond_wait(&configuration->conditionSignal, &configuration->syncMutex);

		printf("%s: Processing opcode=%d\r\n", configuration->name, configuration->opcode);

		/*Execute command*/
		switch (configuration->opcode)
		{
			case OPCODE_GET_IMAGE:
				getImage(configuration);
				break;
			case OPCODE_SET_GAIN_AUTO:
				setGainAuto(configuration->camera, configuration->gainAuto);
				break;
			case OPCODE_GET_GAIN_AUTO:
				getGainAuto(configuration->camera, &configuration->gainAuto);
				break;
			case OPCODE_SET_GAIN:
				setGain(configuration->camera, configuration->gain);
				break;
			case OPCODE_GET_GAIN:
				getGain(configuration->camera, &configuration->gain);
				break;
			case OPCODE_SET_EXPOSURE:
				setExposure(configuration->camera, configuration->exposure);
				break;
			case OPCODE_GET_EXPOSURE:
				getExposure(configuration->camera, &configuration->exposure);
				break;
			case OPCODE_SET_WIDTH:
				setWidth(configuration->camera, configuration->width);
				break;
			case OPCODE_GET_WIDTH:
				getWidth(configuration->camera, &configuration->width);
				break;
			case OPCODE_SET_HEIGHT:
				setHeight(configuration->camera, configuration->height);
				break;
			case OPCODE_GET_HEIGHT:
				getHeight(configuration->camera, &configuration->height);
				break;
			case OPCODE_SET_OFFSET_X:
				setOffsetX(configuration->camera, configuration->offsetX);
				break;
			case OPCODE_GET_OFFSET_X:
				getOffsetX(configuration->camera, &configuration->offsetX);
				break;
			case OPCODE_SET_OFFSET_Y:
				setOffsetY(configuration->camera, configuration->offsetY);
				break;
			case OPCODE_GET_OFFSET_Y:
				getOffsetY(configuration->camera, &configuration->offsetY);
				break;
			case OPCODE_GET_SIZE:
				getSize(configuration->camera, &configuration->size);
				break;
			case OPCODE_SET_TRIGGER_SOURCE:
				setTriggerSource(configuration->camera, configuration->triggerSource);
				break;
			case OPCODE_GET_TRIGGER_SOURCE:
				getTriggerSource(configuration->camera, &configuration->triggerSource);
				break;
		}

		printf("%s: Finished processing opcode=%d\r\n", configuration->name, configuration->opcode);

		pthread_cond_signal(&configuration->conditionSignal);
	}

	configuration->camera->Close();

	return NULL;
}

long
basler_getImage(basler_t device, uint8_t *buffer, uint32_t size)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	configurations[device].buffer	=	buffer;
	configurations[device].size		=	size;
	configurations[device].opcode	=	OPCODE_GET_IMAGE;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}

long
basler_setGainAuto(basler_t device, bool gainAuto)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	configurations[device].gainAuto	=	gainAuto;
	configurations[device].opcode	=	OPCODE_SET_GAIN_AUTO;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}

long
basler_getGainAuto(basler_t device, bool* gainAuto)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	configurations[device].opcode	=	OPCODE_GET_GAIN_AUTO;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);

	*gainAuto	=	configurations[device].gainAuto;
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}
long
basler_setGain(basler_t device, uint32_t gain)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	configurations[device].gain		=	gain;
	configurations[device].opcode	=	OPCODE_SET_GAIN;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}

long
basler_getGain(basler_t device, uint32_t* gain)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	configurations[device].opcode	=	OPCODE_GET_GAIN;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);

	*gain	=	configurations[device].gain;
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}

long
basler_setExposure(basler_t device, uint32_t exposure)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	configurations[device].exposure	=	exposure;
	configurations[device].opcode	=	OPCODE_SET_EXPOSURE;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}

long
basler_getExposure(basler_t device, uint32_t* exposure)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	configurations[device].opcode	=	OPCODE_GET_EXPOSURE;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);

	*exposure	=	configurations[device].exposure;
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}

long
basler_setWidth(basler_t device, uint32_t width)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	if ((width + configurations[device].offsetX) > 1296)
	{
		errlogPrintf("\x1B[31mBad width/offsetX configuration\r\n\x1B[0m");
		pthread_mutex_unlock(&configurations[device].hardwareMutex);
		return -1;
	}

	configurations[device].width	=	width;
	configurations[device].opcode	=	OPCODE_SET_WIDTH;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}

long
basler_getWidth(basler_t device, uint32_t* width)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	configurations[device].opcode	=	OPCODE_GET_WIDTH;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);

	*width	=	configurations[device].width;
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}

long
basler_setHeight(basler_t device, uint32_t height)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	if ((height + configurations[device].offsetY) > 966)
	{
		errlogPrintf("\x1B[31mBad height/offsetY configuration\r\n\x1B[0m");
		pthread_mutex_unlock(&configurations[device].hardwareMutex);
		return -1;
	}

	configurations[device].height	=	height;
	configurations[device].opcode	=	OPCODE_SET_HEIGHT;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}

long
basler_getHeight(basler_t device, uint32_t* height)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	configurations[device].opcode	=	OPCODE_GET_HEIGHT;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);

	*height	=	configurations[device].height;
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}

long
basler_setOffsetX(basler_t device, uint32_t offsetX)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	if ((offsetX + configurations[device].width) > 1296)
	{
		errlogPrintf("\x1B[31mBad width/offsetX configuration\r\n\x1B[0m");
		pthread_mutex_unlock(&configurations[device].hardwareMutex);
		return -1;
	}

	configurations[device].offsetX	=	offsetX;
	configurations[device].opcode	=	OPCODE_SET_OFFSET_X;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}

long
basler_getOffsetX(basler_t device, uint32_t* offsetX)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	configurations[device].opcode	=	OPCODE_GET_OFFSET_X;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);

	*offsetX	=	configurations[device].offsetX;
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}

long
basler_setOffsetY(basler_t device, uint32_t offsetY)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	if ((offsetY + configurations[device].height) > 966)
	{
		errlogPrintf("\x1B[31mBad height/offsetY configuration\r\n\x1B[0m");
		pthread_mutex_unlock(&configurations[device].hardwareMutex);
		return -1;
	}

	configurations[device].offsetY	=	offsetY;
	configurations[device].opcode	=	OPCODE_SET_OFFSET_Y;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}

long
basler_getOffsetY(basler_t device, uint32_t* offsetY)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	configurations[device].opcode	=	OPCODE_GET_OFFSET_Y;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);

	*offsetY	=	configurations[device].offsetY;
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}
long
basler_getSize(basler_t device, uint32_t* size)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	configurations[device].opcode	=	OPCODE_GET_SIZE;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);

	*size	=	configurations[device].size;
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}

long
basler_setTriggerSource(basler_t device, triggerSource_t source)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	configurations[device].triggerSource	=	source;
	configurations[device].opcode			=	OPCODE_SET_TRIGGER_SOURCE;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}

long
basler_getTriggerSource(basler_t device, triggerSource_t* source)
{
	/*Lock camera*/
	pthread_mutex_lock(&configurations[device].hardwareMutex);

	configurations[device].opcode	=	OPCODE_GET_TRIGGER_SOURCE;

	/*Synchronize*/
	pthread_mutex_lock(&configurations[device].syncMutex);
	pthread_cond_signal(&configurations[device].conditionSignal);
	pthread_cond_wait(&configurations[device].conditionSignal, &configurations[device].syncMutex);

	*source	=	configurations[device].triggerSource;
	pthread_mutex_unlock(&configurations[device].syncMutex);

	/*Unlock camera*/
	pthread_mutex_unlock(&configurations[device].hardwareMutex);

	return 0;
}


static void
getImage(configuration_t* configuration)
{
	configuration->streamGrabber->MaxBufferSize.SetValue(configuration->size);
	configuration->streamGrabber->MaxNumBuffer.SetValue(1);

	configuration->streamGrabber->PrepareGrab();
	const StreamBufferHandle hBuffer = configuration->streamGrabber->RegisterBuffer(configuration->buffer, configuration->size);
	configuration->streamGrabber->QueueBuffer(hBuffer, NULL);

	configuration->camera->AcquisitionStart.Execute();
	if (configuration->triggerSource == TRIGGER_SOURCE_SOFTWARE)
		configuration->camera->TriggerSoftware.Execute();

	if (configuration->streamGrabber->GetWaitObject().Wait(3000))
	{
		GrabResult Result;
		configuration->streamGrabber->RetrieveResult(Result);

		if (Result.Succeeded())
			printf("Gray value of first pixel: %d\r\n", (uint32_t) configuration->buffer[0]);
		else
			printf("Error code : %x\r\n", Result.GetErrorCode());
	}
	else 
	{
		printf("Timeout occurred!\r\n");
		configuration->streamGrabber->CancelGrab();
		for (GrabResult r; configuration->streamGrabber->RetrieveResult(r););
	}
	configuration->streamGrabber->DeregisterBuffer(hBuffer);
	configuration->streamGrabber->FinishGrab();
}

static void
setGainAuto(CBaslerGigECamera* camera, bool gainAuto)
{
	if (gainAuto)
		camera->GainAuto.SetValue(GainAuto_Continuous);
	else
		camera->GainAuto.SetValue(GainAuto_Off);
}

static void
getGainAuto(CBaslerGigECamera* camera, bool* gainAuto)
{
	int gainAutoEnum;

	gainAutoEnum	=	camera->GainAuto.GetValue();
	if (gainAutoEnum == GainAuto_Off)
		*gainAuto	=	false;
	else
		*gainAuto	=	true;
}

static void
setGain(CBaslerGigECamera* camera, uint32_t gain)
{
	int	gainAutoEnum;

	gainAutoEnum	=	camera->GainAuto.GetValue();
	if (gainAutoEnum == GainAuto_Off)
	{
		camera->GainSelector.SetValue(GainSelector_All);
		camera->GainRaw.SetValue(gain);
	}
}

static void
getGain(CBaslerGigECamera* camera, uint32_t* gain)
{
	*gain	=	camera->GainRaw.GetValue();
}

static void
setExposure(CBaslerGigECamera* camera, uint32_t exposure)
{
	if (exposure < 16)
		exposure = 16;

	camera->ExposureMode.SetValue(ExposureMode_Timed);
	camera->ExposureTimeAbs.SetValue(exposure);
}

static void
getExposure(CBaslerGigECamera* camera, uint32_t* exposure)
{
	*exposure	=	camera->ExposureTimeAbs.GetValue();
}

static void
setWidth(CBaslerGigECamera* camera, uint32_t width)
{
	camera->Width.SetValue(width);
}

static void
getWidth(CBaslerGigECamera* camera, uint32_t* width)
{
	*width	=	camera->Width.GetValue();
}

static void
setHeight(CBaslerGigECamera* camera, uint32_t height)
{
	camera->Height.SetValue(height);
}

static void
getHeight(CBaslerGigECamera* camera, uint32_t* height)
{
	*height	=	camera->Height.GetValue();
}

static void
setOffsetX(CBaslerGigECamera* camera, uint32_t offsetX)
{
	camera->OffsetX.SetValue(offsetX);
}

static void
getOffsetX(CBaslerGigECamera* camera, uint32_t* offsetX)
{
	*offsetX	=	camera->OffsetX.GetValue();
}

static void
setOffsetY(CBaslerGigECamera* camera, uint32_t offsetY)
{
	camera->OffsetY.SetValue(offsetY);
}

static void
getOffsetY(CBaslerGigECamera* camera, uint32_t* offsetY)
{
	*offsetY	=	camera->OffsetY.GetValue();
}

static void
getSize(CBaslerGigECamera* camera, uint32_t* size)
{
	*size	=	camera->Width.GetValue()*camera->Height.GetValue();
}

static void
setTriggerSource(CBaslerGigECamera* camera, triggerSource_t source)
{
	switch (source)
	{
		case TRIGGER_SOURCE_SOFTWARE:
			camera->TriggerSource.SetValue(TriggerSource_Software);
			break;
		case TRIGGER_SOURCE_HARDWARE:
			camera->TriggerSource.SetValue(TriggerSource_Line1);
			camera->TriggerActivation.SetValue(TriggerActivation_RisingEdge);
			break;
		default:
			camera->TriggerSource.SetValue(TriggerSource_Software);
			break;
	}
}

static void
getTriggerSource(CBaslerGigECamera* camera, triggerSource_t* source)
{
	switch (camera->TriggerSource.GetValue())
	{
		case TriggerSource_Software:
			*source	=	TRIGGER_SOURCE_SOFTWARE;
			break;
		case TriggerSource_Line1:
			*source	=	TRIGGER_SOURCE_HARDWARE;
			break;
		default:
			*source	=	TRIGGER_SOURCE_SOFTWARE;
			break;
	}
}

static long
report(int detail)
{
	int device;

	if (detail || !detail)
	{
		for (device = 0; device < deviceCount; device++)
			printf("Found %s @ %s\r\n", configurations[device].name, configurations[device].ip);
	}
	return 0;
}

/*
 * Configuration and registration functions and variables
 */
static 	const 	iocshArg		configureArg0 	= 	{ "name",	iocshArgString };
static 	const 	iocshArg		configureArg1 	= 	{ "ip", 	iocshArgString };
static 	const 	iocshArg*		configureArgs[] = 
{
    &configureArg0,
    &configureArg1
};
static	const	iocshFuncDef	configureDef	=	{ "baslerConfigure", 2, configureArgs };
static 	long	configure(char *name, char *ip)
{
	char	buffer[INET_ADDRSTRLEN];

	if (!name || strlen(name) == 0)
	{
		errlogPrintf("\x1B[31mUnable to configure device: Missing name\r\n\x1B[0m");
		return -1;
	}
	if (!ip || !strlen(ip) || inet_pton(AF_INET, ip, buffer) < 0)
	{
		errlogPrintf("\x1B[31mUnable to configure device: Missing or incorrect IP address\r\n\x1B[0m");
		return -1;
	}
	if (deviceCount >= NUMBER_OF_DEVICES)
	{
		errlogPrintf("\x1B[31mUnable to configure device: Too many devices\r\n\x1B[0m");
		return -1;
	}

	strcpy(configurations[deviceCount].name, name);
	strcpy(configurations[deviceCount].ip, ip);

	deviceCount++;

	return 0;
}

static void configureFunc (const iocshArgBuf *args)
{
    configure(args[0].sval, args[1].sval);
}

static void baslerRegister(void)
{
	iocshRegister(&configureDef, configureFunc);
}

/*
 * Registry export
 */
static drvet drvBasler = {
    2,
    NULL,
    init
};
epicsExportAddress(drvet, drvBasler);
epicsExportRegistrar(baslerRegister);
