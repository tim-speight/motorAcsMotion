#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <algorithm>
#include <string>
#include <sstream>
#include <cstdarg>

#include <iocsh.h>
#include <epicsThread.h>

#include <asynOctetSyncIO.h>

#include "asynDriver.h"
#include "asynMotorController.h"
#include "asynMotorAxis.h"

#include <epicsExport.h>

#include "SPiiPlusDriver.h"
#include "SPiiPlusBinComm.h"

static const char *driverName = "SPiiPlusController";

static void SPiiPlusProfileThreadC(void *pPvt);

#ifndef MAX
#define MAX(a,b) ((a)>(b)? (a): (b))
#endif
#ifndef MIN
#define MIN(a,b) ((a)<(b)? (a): (b))
#endif

SPiiPlusController::SPiiPlusController(const char* ACSPortName, const char* asynPortName, int numAxes,
                                             double movingPollPeriod, double idlePollPeriod)
 : asynMotorController(ACSPortName, numAxes, NUM_SPIIPLUS_PARAMS, 0, 0, ASYN_CANBLOCK | ASYN_MULTIDEVICE, 1, 0, 0)
{
	asynStatus status = pasynOctetSyncIO->connect(asynPortName, 0, &pasynUserController_, NULL);
	pAxes_ = (SPiiPlusAxis **)(asynMotorController::pAxes_);
	std::stringstream cmd;
	
	// Create parameters
	createParam(SPiiPlusHomingMethodString,               asynParamInt32, &SPiiPlusHomingMethod_);
	createParam(SPiiPlusTestString,                       asynParamInt32, &SPiiPlusTest_);
	
	if (status)
	{
		asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
		"SPiiPlusController::SPiiPlusController: cannot connect to SPii+ controller\n");
		
		return;
	}
	
	for (int index = 0; index < numAxes; index += 1)
	{
		new SPiiPlusAxis(this, index);
		
		// Query the MFLAGS
		cmd << "?D/MFLAGS(" << index << ")";
		writeReadInt(cmd, &(pAxes_[index]->mflags_));
		// Bit 0 is #DUMMY
		pAxes_[index]->dummy_ = (pAxes_[index]->mflags_) & (1 << 0);
		// Bit 4 is #STEPPER
		pAxes_[index]->stepper_ = (pAxes_[index]->mflags_) & (1 << 4);
		// Bit 5 is #ENCLOOP
		pAxes_[index]->encloop_ = (pAxes_[index]->mflags_) & (1 << 5);
		// Bit 6 is #STEPENC
		pAxes_[index]->stepenc_ = (pAxes_[index]->mflags_) & (1 << 6);
		
		// Query the axis resolution (used to convert motor record steps into controller EGU)
		cmd << "?STEPF(" << index << ")";
		writeReadDouble(cmd, &(pAxes_[index]->resolution_));
		
		// Query the encoder resolution (not currently used for anything)
		cmd << "?EFAC(" << index << ")";
		writeReadDouble(cmd, &(pAxes_[index]->encoderResolution_));
		// Query the encoder offset (not currently used for anything)
		cmd << "?EOFFS(" << index << ")";
		writeReadDouble(cmd, &(pAxes_[index]->encoderOffset_));
	}
	
	this->startPoller(movingPollPeriod, idlePollPeriod, 2);
	
	// Create the event that wakes up the thread for profile moves
	profileExecuteEvent_ = epicsEventMustCreate(epicsEventEmpty);
	
	// Create the thread that will execute profile moves
	epicsThreadCreate("SPiiPlusProfile", 
		epicsThreadPriorityLow,
		epicsThreadGetStackSize(epicsThreadStackMedium),
		(EPICSTHREADFUNC)SPiiPlusProfileThreadC, (void *)this);
}

asynStatus SPiiPlusController::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
  int function = pasynUser->reason;
  int status = asynSuccess;
  SPiiPlusAxis *pAxis;
  static const char *functionName = "writeInt32";

  pAxis = this->getAxis(pasynUser);
  if (!pAxis) return asynError;
  
  /* Set the parameter and readback in the parameter library.  This may be overwritten when we read back the
   * status at the end, but that's OK */
  status = pAxis->setIntegerParam(function, value);
  
  if (function == SPiiPlusHomingMethod_)
  {
    //
    asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "%s:%s: homingMethod = %i\n", driverName, functionName, value);
  }
  
  if (function == SPiiPlusTest_) {
    /* Do something helpful during development */
    status = test();
  } else {
    /* Call base class method */
    status = asynMotorController::writeInt32(pasynUser, value);
  }

  /* Do callbacks so higher layers see any changes */
  pAxis->callParamCallbacks();

  return (asynStatus)status;

}

SPiiPlusAxis* SPiiPlusController::getAxis(asynUser *pasynUser)
{
	return static_cast<SPiiPlusAxis*>(asynMotorController::getAxis(pasynUser));
}

SPiiPlusAxis* SPiiPlusController::getAxis(int axisNo)
{
	return static_cast<SPiiPlusAxis*>(asynMotorController::getAxis(axisNo));
}

asynStatus SPiiPlusController::writeReadInt(std::stringstream& cmd, int* val)
{
	static const char *functionName = "writeReadInt";
	char inString[MAX_CONTROLLER_STRING_SIZE];
	std::stringstream val_convert;
	int errNo;

	std::fill(inString, inString + 256, '\0');
	
	size_t response;
	asynStatus status = this->writeReadController(cmd.str().c_str(), inString, 256, &response, -1);
	
	asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "%s:%s: output = %s\n", driverName, functionName, cmd.str().c_str());
	asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "%s:%s:  input = %s\n", driverName, functionName, inString);
	asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "%s:%s: status = %i\n", driverName, functionName, status);
	
	if (status == asynSuccess)
	{
		if (inString[0] != '?')
		{
			// inString ends with \r:\r, but that isn't a problem for the following conversion
			val_convert << std::string(inString);
			val_convert >> *val;
			
			asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "%s:%s:    val = %i\n", driverName, functionName, *val);
		}
		else
		{
			// Overwrite the '?' so the conversion can succeed
			inString[0] = ' ';
			val_convert << std::string(inString);
			val_convert >> errNo;
			
			asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s:%s: ERROR #%i - command: %s\n", driverName, functionName, errNo, cmd.str().c_str());
			
			status = asynError;
		}
	}
	
	// clear the command stringstream
	cmd.str("");
	cmd.clear();
	
	return status;
}

asynStatus SPiiPlusController::writeReadDouble(std::stringstream& cmd, double* val)
{
	static const char *functionName = "writeReadDouble";
	char inString[MAX_CONTROLLER_STRING_SIZE];
	std::stringstream val_convert;
	int errNo;
	
	std::fill(inString, inString + 256, '\0');
	
	size_t response;
	asynStatus status = this->writeReadController(cmd.str().c_str(), inString, 256, &response, -1);
	
	asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "%s:%s: output = %s\n", driverName, functionName, cmd.str().c_str());
	asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "%s:%s:  input = %s\n", driverName, functionName, inString);
	asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "%s:%s: status = %i\n", driverName, functionName, status);
	
	
	if (status == asynSuccess)
	{
		if (inString[0] != '?')
		{
			// inString ends with \r:\r, but that isn't a problem for the following conversion
			val_convert << std::string(inString);
			val_convert >> *val;
			
			asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "%s:%s:    val = %lf\n", driverName, functionName, *val);
		}
		else
		{
			// Overwrite the '?' so the conversion can succeed
			inString[0] = ' ';
			val_convert << std::string(inString);
			val_convert >> errNo;
			
			asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s:%s: ERROR #%i - command: %s\n", driverName, functionName, errNo, cmd.str().c_str());
			
			status = asynError;
		}
	}
	
	// clear the command stringstream -- this doesn't work
	cmd.str("");
	cmd.clear();
	
	return status;
}

asynStatus SPiiPlusController::writeReadAck(std::stringstream& cmd)
{
	static const char *functionName = "writeReadAck";
	char inString[MAX_CONTROLLER_STRING_SIZE];
	std::stringstream val_convert;
	int errNo;

	std::fill(inString, inString + 256, '\0');
	
	size_t response;
	asynStatus status = this->writeReadController(cmd.str().c_str(), inString, 256, &response, -1);
	
	asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "%s:%s: output = %s\n", driverName, functionName, cmd.str().c_str());
	asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "%s:%s:  input = %s\n", driverName, functionName, inString);
	asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "%s:%s: status = %i\n", driverName, functionName, status);
	
	if (inString[0] == '?')
	{
		// Overwrite the '?' so the conversion can succeed
		inString[0] = ' ';
		val_convert << std::string(inString);
		val_convert >> errNo;
		
		asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s:%s: ERROR #%i - command: %s\n", driverName, functionName, errNo, cmd.str().c_str());
		
		status = asynError;
	}
	
	// clear the command stringstream
	cmd.str("");
	cmd.clear();
	
	return status;
}

// NOTE: readBytes the number of data bytes that were read, excluding the command header and suffix
// NOTE: there is no error checking on inBytes and outBytes
asynStatus SPiiPlusController::writeReadBinary(char *output, int outBytes, char *input, int inBytes, size_t *dataBytes, bool *sliceAvailable)
{
	char* packetBuffer;
	size_t nwrite, nread;
	int eomReason;
	asynStatus status;
	static const char *functionName = "writeReadBinary";
	
	lock();
	
	std::fill(outString_, outString_ + MAX_CONTROLLER_STRING_SIZE, '\0');
	packetBuffer = (char *)calloc(MAX_PACKET_DATA+5, sizeof(char));
	
	// Clear the EOS characters
	pasynOctetSyncIO->setInputEos(pasynUserController_, "", 0);
	pasynOctetSyncIO->setOutputEos(pasynUserController_, "", 0);
	
	// Flush the receive buffer
	status = pasynOctetSyncIO->flush(pasynUserController_);
	
	// Send the query command
	memcpy(outString_, output, outBytes);
	status = pasynOctetSyncIO->write(pasynUserController_, outString_, outBytes, SPIIPLUS_CMD_TIMEOUT, &nwrite);
	
	// The reply from the controller has a 4-byte header and a 1-byte suffix
	status = pasynOctetSyncIO->read(pasynUserController_, packetBuffer, inBytes, SPIIPLUS_ARRAY_TIMEOUT, &nread, &eomReason);
	
	if (status == asynSuccess)
	{
		// Check for an error reply
		status = binaryErrorCheck(packetBuffer);
		if (status == asynError)
		{
			*sliceAvailable = false;
			asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s:%s: No more slices due to error\n", driverName, functionName);
		}
		else
		{
			// Check if there is another slice
			/*
			 * Bit 7 of the 3rd byte, which is the most-significant byte of the BE message size, indicates if another slice is available
			 */
			if (packetBuffer[3] & SLICE_AVAILABLE)
			{
				*sliceAvailable = true;
			}
			else
			{
				*sliceAvailable = false;
			}
			
			// Subtract the 5 header bytes to get the number of bytes in the data
			*dataBytes = nread - 5;
			
			// The data is already in little-endian format, so just copy it
			memcpy(input, packetBuffer+4, *dataBytes);
		}
	}
	else
	{
		*sliceAvailable = false;
		asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s:%s: No more slices due to a failed read: status=%i, nread=%li\n", driverName, functionName, status, nread);
	}
	
	// Restore the EOS characters
	pasynOctetSyncIO->setInputEos(pasynUserController_, "\r", 1);
	pasynOctetSyncIO->setOutputEos(pasynUserController_, "\r", 1);
	
	unlock();
	
	return status;
	}

asynStatus SPiiPlusController::binaryErrorCheck(char *buffer)
{
	asynStatus status=asynSuccess;
	std::stringstream val_convert;
	int errNo;
	static const char *functionName = "binaryErrorCheck";
	
	// If the first character of the data is a question mark, the error number follows it
	if ((buffer[4] == 0x3f) && (buffer[9] == 0x0d))
	{
		/*
		 *  Error response: [E3][XX][06][00]?####[0D][E6]
		 */
		 
		// replace the carriage return with a null byte
		buffer[9] = 0;
		
		// convert the error number bytes into an int
		val_convert << buffer+5;
		val_convert >> errNo;
		
		asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s:%s: Binary command error #%i\n", driverName, functionName, errNo);
		status = asynError;
	}
	
	return status;
}

asynStatus SPiiPlusController::getDoubleArray(char *output, const char *var, int idx1start, int idx1end, int idx2start, int idx2end)
{
	char command[MAX_MESSAGE_LEN];
	asynStatus status;
	int remainingBytes;
	int readBytes;
	int outBytes, inBytes, dataBytes;
	size_t nread;
	int slice=1;
	bool sliceAvailable;
	static const char *functionName = "getDoubleArray";
	
	std::fill(outString_, outString_ + MAX_CONTROLLER_STRING_SIZE, '\0');
	
	// Create the command to query array data. This could be the only command
	// that needs to be sent or it could be the first of many.
	readFloat64ArrayCmd(command, var, idx1start, idx1end, idx2start, idx2end, &outBytes, &inBytes, &dataBytes);
	
	remainingBytes = dataBytes;
	readBytes = 0;
	
	// Send the command
	status = writeReadBinary((char*)command, outBytes, output+readBytes, inBytes, &nread, &sliceAvailable);
	asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s:%s: Initial array query: request = %i; read = %li\n", driverName, functionName, inBytes, nread);
	
	remainingBytes -= nread;
	readBytes += nread;
	
	// Look at the response to see if there are more slices to read
	while (sliceAvailable)
	{
		// Create the command to query the next slice of the array data
		readFloat64SliceCmd(command, slice, var, idx1start, idx1end, idx2start, idx2end, &outBytes, &inBytes, &dataBytes);
		
		// Send the command
		status = writeReadBinary((char*)command, outBytes, output+readBytes, inBytes, &nread, &sliceAvailable);
		asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s:%s: Array slice #%i query: expected = %i; read = %li; sliceAvailable = %d\n", driverName, functionName, slice, inBytes, nread, sliceAvailable);
		
		remainingBytes -= nread;
		readBytes += nread;
		slice++;
	}
	
	// Restore the EOS characters
	pasynOctetSyncIO->setInputEos(pasynUserController_, "\r", 1);
	pasynOctetSyncIO->setOutputEos(pasynUserController_, "\r", 1);
	
	return status;
}

SPiiPlusAxis::SPiiPlusAxis(SPiiPlusController *pC, int axisNo)
: asynMotorAxis(pC, axisNo),
  pC_(pC)
{
	setIntegerParam(pC->motorStatusHasEncoder_, 1);
	// Gain Support is required for setClosedLoop to be called
	setIntegerParam(pC->motorStatusGainSupport_, 1);
}

asynStatus SPiiPlusAxis::poll(bool* moving)
{
	asynStatus status;
	SPiiPlusController* controller = (SPiiPlusController*) pC_;
	//static const char *functionName = "poll";
	std::stringstream cmd;
	
	double position;
	cmd << "?APOS(" << axisNo_ << ")";
	status = controller->writeReadDouble(cmd, &position);
	if (status != asynSuccess) return status;
	setDoubleParam(controller->motorPosition_, (position / resolution_));
	
	if (dummy_)
	{
		setDoubleParam(controller->motorEncoderPosition_, (position / resolution_));
		// Faults are disabled for dummy axes
		setIntegerParam(controller->motorStatusLowLimit_, 0);
		setIntegerParam(controller->motorStatusHighLimit_, 0);
	}
	else
	{
		double enc_position;
		cmd << "?FPOS(" << axisNo_ << ")";
		status = controller->writeReadDouble(cmd, &enc_position);
		if (status != asynSuccess) return status;
		// FPOS = FP * EFAC + EOFFS
		// TODO: detect when there is no encoder and fix the encoder position at zero
		setDoubleParam(controller->motorEncoderPosition_, enc_position / encoderResolution_);
		
		int left_limit, right_limit;
		cmd << "?FAULT(" << axisNo_ << ").#LL";
		status = controller->writeReadInt(cmd, &left_limit);
		if (status != asynSuccess) return status;
		setIntegerParam(controller->motorStatusLowLimit_, left_limit);
		cmd << "?FAULT(" << axisNo_ << ").#RL";
		status = controller->writeReadInt(cmd, &right_limit);
		if (status != asynSuccess) return status;
		setIntegerParam(controller->motorStatusHighLimit_, right_limit);
	}
	
	// Read the axis status
	int axis_status;
	cmd << "?D/AST(" << axisNo_ << ")";
	status = controller->writeReadInt(cmd, &axis_status);
	if (status != asynSuccess) return status;
	
	int enabled;
	//int open_loop;
	//int in_pos;
	int motion;
	
	if (dummy_)
	{
		enabled = 0;
		motion = axis_status & (1<<5);
	}
	else
	{
		// Read the entire motor status and parse the relevant bits
		int motor_status;
		cmd << "?D/MST(" << axisNo_ << ")";
		status = controller->writeReadInt(cmd, &motor_status);
		if (status != asynSuccess) return status;
	
		enabled = motor_status & (1<<0);
		//open_loop = axis_status & (1<<1);
		//in_pos = axis_status & (1<<4);
		motion = motor_status & (1<<5);
	}
	
	setIntegerParam(controller->motorStatusDone_, !motion);
	setIntegerParam(controller->motorStatusMoving_, motion);
	setIntegerParam(controller->motorStatusPowerOn_, enabled);
	
	callParamCallbacks();
	
	moving_ = *moving;
	
	if (motion)    { *moving = true; }
	else           { *moving = false; }
	
	return status;
}

asynStatus SPiiPlusAxis::move(double position, int relative, double minVelocity, double maxVelocity, double acceleration)
{
	SPiiPlusController* controller = (SPiiPlusController*) pC_;
	asynStatus status;
	double deviceUnits;
	std::stringstream cmd;
	
	// Should accelerations be multiplied by resolution_ squared?
	//cmd << "XACC(" << axisNo_ << ")=" << ((acceleration + 10) * resolution_);
	//status = controller->writeReadAck(cmd);
	cmd << "ACC(" << axisNo_ << ")=" << (acceleration * resolution_);
	status = controller->writeReadAck(cmd);
	cmd << "DEC(" << axisNo_ << ")=" << (acceleration * resolution_);
	status = controller->writeReadAck(cmd);
	
	//cmd << "XVEL(" << axisNo_ << ")=" << ((maxVelocity + 10) * resolution_);
	//status = controller->writeReadAck(cmd);
	cmd << "VEL(" << axisNo_ << ")=" << (maxVelocity * resolution_);
	status = controller->writeReadAck(cmd);
	
	if (relative)
	{
		cmd << "PTP/r " << axisNo_ << ", " << (position * resolution_);
		status = controller->writeReadAck(cmd);
	}
	else
	{
		cmd << "PTP " << axisNo_ << ", " << (position * resolution_);
		status = controller->writeReadAck(cmd);
	}
	
	return status;
}

asynStatus SPiiPlusAxis::setPosition(double position)
{
	SPiiPlusController* controller = (SPiiPlusController*) pC_;
	asynStatus status;
	std::stringstream cmd;
	
	cmd << "SET APOS(" << axisNo_ << ")=" << (position * resolution_);
	status = controller->writeReadAck(cmd);
	
	return status;
}

asynStatus SPiiPlusAxis::stop(double acceleration)
{
	SPiiPlusController* controller = (SPiiPlusController*) pC_;
	asynStatus status;
	std::stringstream cmd;
	
	cmd << "HALT " << axisNo_;
	status = controller->writeReadAck(cmd);
	
	return status;
}

/** Set the motor closed loop status. 
  * \param[in] closedLoop true = close loop, false = open looop. */
asynStatus SPiiPlusAxis::setClosedLoop(bool closedLoop)
{
	SPiiPlusController* controller = (SPiiPlusController*) pC_;
	asynStatus status=asynSuccess;
	std::stringstream cmd;
	
	if (!dummy_)
	{
		/*
		Enable/disable the axis instead of changing the closed-loop state.
		*/
		if (closedLoop)
		{
			cmd << "ENABLE " << axisNo_;
		}
		else
		{
			cmd << "DISABLE " << axisNo_;
		}
		status = controller->writeReadAck(cmd);
	}
	
	return status;
}

/** Move the motor to the home position.
  * \param[in] minVelocity The initial velocity, often called the base velocity. Units=steps/sec.
  * \param[in] maxVelocity The maximum velocity, often called the slew velocity. Units=steps/sec.
  * \param[in] acceleration The acceleration value. Units=steps/sec/sec.
  * \param[in] forwards  Flag indicating to move the motor in the forward direction(1) or reverse direction(0).
  *                      Some controllers need to be told the direction, others know which way to go to home. */
asynStatus SPiiPlusAxis::home(double minVelocity, double maxVelocity, double acceleration, int forwards)
{
	SPiiPlusController* controller = (SPiiPlusController*) pC_;
	asynStatus status=asynSuccess;
	std::stringstream cmd;
	epicsInt32 mbboHomingMethod;
	epicsInt32 homingMethod;
	static const char *functionName = "home";
	
	controller->getIntegerParam(axisNo_, controller->SPiiPlusHomingMethod_, &mbboHomingMethod);
	
	switch(mbboHomingMethod)
	{
		case MBBO_HOME_NONE:
			homingMethod = SPIIPLUS_HOME_NONE;
			break;
		
		case MBBO_HOME_LIMIT_INDEX:
			if (forwards == 0)
				homingMethod = SPIIPLUS_HOME_NEG_LIMIT_INDEX;
			else
				homingMethod = SPIIPLUS_HOME_POS_LIMIT_INDEX;
			break;
		
		case MBBO_HOME_LIMIT:
			if (forwards == 0)
				homingMethod = SPIIPLUS_HOME_NEG_LIMIT;
			else
				homingMethod = SPIIPLUS_HOME_POS_LIMIT;
			break;
		
		case MBBO_HOME_INDEX:
			if (forwards == 0)
				homingMethod = SPIIPLUS_HOME_NEG_INDEX;
			else
				homingMethod = SPIIPLUS_HOME_POS_INDEX;
			break;
		
		case MBBO_HOME_CURRENT_POS:
			homingMethod = SPIIPLUS_HOME_CURRENT_POS;
			break;
		
		case MBBO_HOME_HARDSTOP_INDEX:
			if (forwards == 0)
				homingMethod = SPIIPLUS_HOME_NEG_HARDSTOP_INDEX;
			else
				homingMethod = SPIIPLUS_HOME_POS_HARDSTOP_INDEX;
			break;
		
		case MBBO_HOME_HARDSTOP:
			if (forwards == 0)
				homingMethod = SPIIPLUS_HOME_NEG_HARDSTOP;
			else
				homingMethod = SPIIPLUS_HOME_POS_HARDSTOP;
			break;
		
		case MBBO_HOME_CUSTOM:
			//if (forwards == 0)
			//	???
			//else
			//	???
			// Do nothing for now
			homingMethod = SPIIPLUS_HOME_NONE;
			break;
		
		default:
			homingMethod = SPIIPLUS_HOME_NONE;
			break;
	}
	
	if (homingMethod == SPIIPLUS_HOME_NONE)
	{
		asynPrint(pC_->pasynUserSelf, ASYN_TRACE_ERROR, "%s:%s: No homing method selected for axis %i\n", driverName, functionName, axisNo_);
		// Should status be set to asynError here?
	}
	else
	{
		// HOME Axis, [opt]HomingMethod,[opt]HomingVel,[opt]MaxDistance,[opt]HomingOffset,[opt]HomingCurrLimit,[opt]HardStopThreshold
		cmd << "HOME " << axisNo_ << "," << homingMethod << "," << (maxVelocity * resolution_);
		//asynPrint(pC_->pasynUserSelf, ASYN_TRACE_ERROR, "%s:%s: home command = %s\n", driverName, functionName, cmd.str().c_str());
		status = controller->writeReadAck(cmd);
	}
	return status;
}

/** Function to define the motor positions for a profile move. 
  * Called by asynMotorController::writeFloat64Array
  * This calls the base class defineProfile method to convert to steps, but since the
  * SPiiPlus works in user-units we need to do an additional conversion by resolution_. 
  * \param[in] positions Array of profile positions for this axis in user units.
  * \param[in] numPoints The number of positions in the array.
  */
asynStatus SPiiPlusAxis::defineProfile(double *positions, size_t numPoints)
{
  size_t i;
  asynStatus status;
  //static const char *functionName = "defineProfile";
  
  // Call the base class function (converts from EGU to steps)
  status = asynMotorAxis::defineProfile(positions, numPoints);
  if (status) return status;
  
  // Convert from steps to SPiiPlus user units
  for (i=0; i<numPoints; i++) {
    profilePositions_[i] = profilePositions_[i]*resolution_;
  }
  return asynSuccess;
}

/** Reports on status of the axis
  * \param[in] fp The file pointer on which report information will be written
  * \param[in] level The level of report detail desired
  *
  * If details > 0 then information is printed about each axis.
  * After printing controller-specific information it calls asynMotorController::report()
  */
void SPiiPlusAxis::report(FILE *fp, int level)
{
  SPiiPlusController* controller = (SPiiPlusController*) pC_;
  epicsInt32 homingMethod;
  
  controller->getIntegerParam(axisNo_, controller->SPiiPlusHomingMethod_, &homingMethod);
  
  fprintf(fp, "Configuration for axis %i:\n", axisNo_);
  fprintf(fp, "  mflags: %i\n", mflags_);
  fprintf(fp, "    dummy:  %i\n", dummy_);
  fprintf(fp, "    stepper:  %i\n", stepper_);
  fprintf(fp, "    encloop:  %i\n", encloop_);
  fprintf(fp, "    stepenc:  %i\n", stepenc_);
  fprintf(fp, "  moving: %i\n", moving_);
  fprintf(fp, "  resolution: %.6e\n", resolution_);
  fprintf(fp, "  encoder resolution: %.6e\n", encoderResolution_);
  fprintf(fp, "  encoder offset: %lf\n", encoderOffset_);
  fprintf(fp, "  homing method: %i\n", homingMethod);
  fprintf(fp, "\n");
  
  // Call the base class method
  asynMotorAxis::report(fp, level);
}

std::string SPiiPlusController::axesToString(std::vector <int> axes)
{
  //static const char *functionName = "axesToString";
  unsigned int i;
  std::stringstream outputStr;
  
  for (i=0; i<axes.size(); i++)
  {
    if (axes[i] == axes.front())
    {
      if (axes.size() > 1)
      {
          // Parentheses are only required when multiple axes are used
          outputStr << '(';
      }
      outputStr << axes[i];
    }
    else if (axes[i] == axes.back())
    {
      outputStr << ',' << axes[i] << ')';
    }
    else
    {
      outputStr << ',' << axes[i];
    }
  }
  
  return outputStr.str();
}

// Create a motor list string to be displayed in a message to the user
std::string SPiiPlusController::motorsToString(std::vector <int> axes)
{
  //static const char *functionName = "motorsToString";
  unsigned int i;
  std::stringstream outputStr;
  
  for (i=0; i<axes.size(); i++)
  {
    if (axes[i] == axes.front())
    {
      outputStr << (axes[i]+1);
    }
    else
    {
      outputStr << ", " << (axes[i]+1);
    }
  }
  
  return outputStr.str();
}

std::string SPiiPlusController::positionsToString(int positionIndex)
{
  //static const char *functionName = "positionsToString";
  unsigned int i;
  SPiiPlusAxis *pAxis;
  std::stringstream outputStr;
  
  for (i=0; i<profileAxes_.size(); i++)
  {
    pAxis = getAxis(i);
    
    if (profileAxes_[i] == profileAxes_.front())
    {
      outputStr << pAxis->fullProfilePositions_[positionIndex];
    }
    else 
    {
      outputStr << ',' << pAxis->fullProfilePositions_[positionIndex];
    }
  }
  
  return outputStr.str();
}


asynStatus SPiiPlusController::initializeProfile(size_t maxProfilePoints)
{
  int axis;
  SPiiPlusAxis *pAxis;
  asynStatus status;
  int i;
  std::stringstream cmd;
  // static const char *functionName = "initializeProfile";
  
  /*
   * Create point and time arrays that have extra elements for the 
   * acceleration and deceleration, not including the starting position
   */
  if (fullProfileTimes_) free(fullProfileTimes_);
  fullProfileTimes_ = (double *)calloc(maxProfilePoints+(2*MAX_ACCEL_SEGMENTS)-1, sizeof(double));
  for (axis=0; axis<numAxes_; axis++) {
    pAxis = getAxis(axis);
    if (!pAxis) continue;
    if (pAxis->fullProfilePositions_) free(pAxis->fullProfilePositions_);
    pAxis->fullProfilePositions_ = (double *)calloc(maxProfilePoints+(2*MAX_ACCEL_SEGMENTS)-1, sizeof(double));
  }
  status = asynMotorController::initializeProfile(maxProfilePoints);
  
  // Create the arrays in the controller to hold the data that is recorded during profile moves
  for (i=0; i<SPIIPLUS_MAX_DC_AXES; i++)
  {
    // Delete the array, if it exists, in case maxProfilePoints changed
    cmd << "#VGV DC_DATA_" << (i+1);
    writeReadAck(cmd);
    
    // Data recorded with the DC command will reside in DC_DATA_{1,2,3,4,5,6,7,8} 2D arrays
    cmd << "GLOBAL REAL DC_DATA_" << (i+1) << " (3)(" << maxProfilePoints << ")";
    writeReadAck(cmd);
  }
  
  return status;
}

/** Function to build a coordinated move of multiple axes. */
asynStatus SPiiPlusController::buildProfile()
{
  int i; 
  unsigned int j; 
  int status;
  bool buildOK=true;
  //bool verifyOK=true;
  int numPoints;
  //int numElements;
  //double trajVel;
  //double D0, D1, T0, T1;
  int moveMode;
  char message[MAX_MESSAGE_LEN];
  int buildStatus;
  double maxVelocity;
  double maxAcceleration;
  //double maxVelocityActual=0.0;
  //double maxAccelerationActual=0.0;
  //double minPositionActual=0.0, maxPositionActual=0.0;
  //double minProfile, maxProfile;
  //double lowLimit, highLimit;
  //double minJerkTime, maxJerkTime;
  double preTimeMax, postTimeMax;
  double preVelocity[SPIIPLUS_MAX_AXES], postVelocity[SPIIPLUS_MAX_AXES];
  double preTime, postTime;
  double preDistance, postDistance;
  double accelTime;
  //int axis
  std::string axisList;
  int useAxis;
  std::stringstream cmd;
  static const char *functionName = "buildProfile";
  
  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
            "%s:%s: entry\n",
            driverName, functionName);
            

  // Call the base class method which will build the time array if needed
  asynMotorController::buildProfile();
  
  strcpy(message, "");
  setStringParam(profileBuildMessage_, message);
  setIntegerParam(profileBuildState_, PROFILE_BUILD_BUSY);
  setIntegerParam(profileBuildStatus_, PROFILE_STATUS_UNDEFINED);
  callParamCallbacks();
  
  // 
  profileAxes_.clear();
  memset(profileAccelTimes_, 0, MAX_ACCEL_SEGMENTS*sizeof(double));
  memset(profileDecelTimes_, 0, MAX_ACCEL_SEGMENTS*sizeof(double));
  
  for (i=0; i<numAxes_; i++) {
    // Zero the velocity arrays
    preVelocity[i] = 0.;
    postVelocity[i] = 0.;
    // Zero the accel/decel arrays
    memset(pAxes_[i]->profileAccelPositions_, 0, MAX_ACCEL_SEGMENTS*sizeof(double));
    memset(pAxes_[i]->profileDecelPositions_, 0, MAX_ACCEL_SEGMENTS*sizeof(double));
    // Check which axes should be used
    getIntegerParam(i, profileUseAxis_, &useAxis);
    asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "%s:%s: %i axis will be used: %i\n", driverName, functionName, i, useAxis);
    if (useAxis)
    {
      profileAxes_.push_back(i);
    }
  }
  
  if (profileAxes_.size() == 0)
  {
    strcpy(message, "No axes selected");
    buildOK = false;
    goto done;
  }
  
  asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "%s:%s: axisList = %s\n", driverName, functionName, axesToString(profileAxes_).c_str());
  sprintf(message, "Selected axes: %s", motorsToString(profileAxes_).c_str()); 
  setStringParam(profileBuildMessage_, message);
  callParamCallbacks();
  
  /* We create trajectories with extra elements at the beginning and at the end.
   * The distance and time of the prepended elements are defined so that the motors will
   * accelerate from 0 to the velocity of the first "real" element in the user-specified
   * acceleration time, as long as it doesn't exceed the maximum allowed acceleration.
   * Similarly, the distance and time of appended elements are defined so that the 
   * motors will decelerate from the velocity of the last "real" element to 0 
   * in the user-specified acceleration time. */

  preTimeMax = 0.;
  postTimeMax = 0.;
  getIntegerParam(profileNumPoints_, &numPoints);
  getDoubleParam(profileAcceleration_, &accelTime);
  getIntegerParam(profileMoveMode_, &moveMode);
  
  for (j=0; j<profileAxes_.size(); j++)
  {
    // Query the max velocity and acceleration
    cmd << "?XVEL(" << j << ")";
    status = writeReadDouble(cmd, &maxVelocity);
    if (status) {
      buildOK = false;
      sprintf(message, "Error getting XVEL, status=%d\n", status);
      goto done;
    }
    cmd << "?XACC(" << j << ")";
    status = writeReadDouble(cmd, &maxAcceleration);
    if (status) {
      buildOK = false;
      sprintf(message, "Error getting XACC, status=%d\n", status);
      goto done;
    }
    
    /* The calculation using maxAcceleration read from controller below
     * is "correct" but subject to roundoff errors when sending ASCII commands.
     * Reduce acceleration 10% to account for this. */
    maxAcceleration *= 0.9;
    
    if (moveMode == PROFILE_MOVE_MODE_ABSOLUTE)
    {
      preDistance = pAxes_[j]->profilePositions_[1] - pAxes_[j]->profilePositions_[0];
    }
    else
    {
      preDistance = pAxes_[j]->profilePositions_[0];
    }
    // Use the 2nd element of the times array instead of the 1st; the 1st will be used for the preDistance move.
    preVelocity[j] = preDistance/profileTimes_[1];
    preTime = fabs(preVelocity[j]) / maxAcceleration;
    preTimeMax = MAX(preTimeMax, preTime);
    // Use the acceleration specified by the user, if it is less than the max acceleration
    preTimeMax = MAX(preTimeMax, accelTime);
    
    if (moveMode == PROFILE_MOVE_MODE_ABSOLUTE)
    {
      postDistance = pAxes_[j]->profilePositions_[numPoints-1] - pAxes_[j]->profilePositions_[numPoints-2];
    }
    else
    {
      postDistance = pAxes_[j]->profilePositions_[numPoints-1];
    }
    postVelocity[j] = postDistance/profileTimes_[numPoints-1];
    postTime = fabs(postVelocity[j]) / maxAcceleration;
    postTimeMax = MAX(postTimeMax, postTime);
    // Use the acceleration specified by the user, if it is less than the max acceleration
    postTimeMax = MAX(postTimeMax, accelTime);
    
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
              "%s:%s: axis %d profilePositions[0]=%f, profilePositions[%d]=%f, maxAcceleration=%f, preTimeMax=%f, postTimeMax=%f\n",
              driverName, functionName, j, pAxes_[j]->profilePositions_[0], numPoints-1, pAxes_[j]->profilePositions_[numPoints-1],
              maxAcceleration, preTimeMax, postTimeMax);
  }
  
  getIntegerParam(profileMoveMode_, &moveMode);
  
  // calculate the number of acceleration segments
  numAccelSegments_ = getNumAccelSegments(preTimeMax);
  numDecelSegments_ = getNumAccelSegments(postTimeMax);
  
  // populate the profileAccelTimes_ and profileDecelTimes_ arrays
  createAccDecTimes(preTimeMax, postTimeMax);
  
  /*
   * Every segment of PATH/POINT/ENDS motion is at a constant velocity.
   * The driver is resposnible for creating the acceleration and 
   * deceleration phases of the full profile move.
   */
  for (j=0; j<profileAxes_.size(); j++)
  {
    pAxes_[j]->profilePreDistance_  =  0.5 * preVelocity[j]  * preTimeMax;
    pAxes_[j]->profilePostDistance_ =  0.5 * postVelocity[j] * postTimeMax;
    
    if (moveMode == PROFILE_MOVE_MODE_ABSOLUTE)
    {
      pAxes_[j]->profileStartPos_ = pAxes_[j]->profilePositions_[0] - pAxes_[j]->profilePreDistance_;
      pAxes_[j]->profileFlybackPos_ = pAxes_[j]->profilePositions_[numPoints-1];
    }
    else
    {
      pAxes_[j]->profileStartPos_ = -pAxes_[j]->profilePreDistance_;
      pAxes_[j]->profileFlybackPos_ = -pAxes_[j]->profilePostDistance_;
    }
    
    // populate the profileAccelPositions_ and profileDecelPositions_ arrays
    createAccDecPositions(pAxes_[j], moveMode, numPoints, preTimeMax, postTimeMax, preVelocity[j], postVelocity[j]);
  }
  
  // populate the fullProfileTimes_ and fullProfilePositions_ arrays
  assembleFullProfile(numPoints);
  
  // calculate the time interval for data collection
  calculateDataCollectionInterval();
  // TODO: clear the data arrays heare instead of in runProfile?
  
  // POINT commands have this syntax: POINT (0,1,5), 1000,2000,3000, 500
  
  // Verfiy the profile (check speed, accel, limit violations)
  
  done:
  buildStatus = (buildOK) ? PROFILE_STATUS_SUCCESS : PROFILE_STATUS_FAILURE;
  setIntegerParam(profileBuildStatus_, buildStatus);
  setStringParam(profileBuildMessage_, message);
  if (buildStatus != PROFILE_STATUS_SUCCESS) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: %s\n",
              driverName, functionName, message);
  }
  else
  {
    // Set the execute and readback status to undefined so that users know those haven't occurred since the build was done
    setIntegerParam(profileExecuteStatus_, PROFILE_STATUS_UNDEFINED);
    setIntegerParam(profileReadbackStatus_, PROFILE_STATUS_UNDEFINED);
  }
  /* Clear build command.  This is a "busy" record, don't want to do this until build is complete. */
  setIntegerParam(profileBuild_, 0);
  setIntegerParam(profileBuildState_, PROFILE_BUILD_DONE);
  callParamCallbacks();
  //return status ? asynError : asynSuccess;
  return asynSuccess;
}

int SPiiPlusController::getNumAccelSegments(double time)
{
  long numSegments;
  // The following value was chosen somewhat arbitrarily -- it gives MAX_ACCEL_SEGMENTS at an acceleration time of 0.2s
  double minPeriod = 0.01;
  
  numSegments = lround(time / minPeriod);
  
  if (numSegments > MAX_ACCEL_SEGMENTS)
  {
    numSegments = MAX_ACCEL_SEGMENTS;
  }
  
  return numSegments;
}

void SPiiPlusController::createAccDecTimes(double preTimeMax, double postTimeMax)
{
  int i;
  //static const char *functionName = "createAccDecTimes";
  
  // Use a constant time for accel/decel segments
  for (i=0; i<numAccelSegments_; i++)
  {
    profileAccelTimes_[i] = preTimeMax / numAccelSegments_;
  }
  for (i=0; i<numDecelSegments_; i++)
  {
    profileDecelTimes_[i] = postTimeMax / numDecelSegments_;
  }
}

void SPiiPlusController::createAccDecPositions(SPiiPlusAxis* axis, int moveMode, int numPoints, double preTimeMax, double postTimeMax, double preVelocity, double postVelocity)
{
  int i;
  double time;
  double time2;
  double time1;
  //static const char *functionName = "createAccDecPositions";
  
  if (moveMode == PROFILE_MOVE_MODE_ABSOLUTE)
  {
    // Acceleration (absolute)
    for (i=0; i<numAccelSegments_; i++)
    {
      time = preTimeMax * (i+1) / numAccelSegments_;
      // position during accel period = starting position of user profile - acceleration distance + distance traveled in i acceleration segments
      axis->profileAccelPositions_[i] = axis->profilePositions_[0] - axis->profilePreDistance_ + 0.5 * (preVelocity / preTimeMax) * pow(time, 2);
    }
    
    // Deceleration (absolute)
    for (i=0; i<numDecelSegments_; i++)
    {
      time = postTimeMax * (i+1) / numDecelSegments_;
      // position during decel period = ending position of user profile + distance traveled in i deceleration segments
      axis->profileDecelPositions_[i] = axis->profilePositions_[numPoints-1] + postVelocity * time - 0.5 * (postVelocity / postTimeMax) * pow(time, 2);
    }
  }
  else
  {
    // Acceleration (relative)
    for (i=0; i<numAccelSegments_; i++)
    {
      // position during accel period = 0.5 * a * (t2^2 - t1^2)
      time2 = preTimeMax*(i+1)/numAccelSegments_;
      time1 = preTimeMax*i/numAccelSegments_;
      axis->profileAccelPositions_[i] = 0.5 * (preVelocity / preTimeMax) * (pow(time2, 2) - pow(time1, 2));
    }
    
    // Deceleration (relative)
    for (i=0; i<numDecelSegments_; i++)
    {
      // position during decel period = vf * (t2 - t1) - 0.5 * a * (t2^2 - t1^2)
      time2 = postTimeMax*(i+1)/numDecelSegments_;
      time1 = postTimeMax*i/numDecelSegments_;
      axis->profileDecelPositions_[i] = postVelocity * (time2 - time1) - 0.5 * (postVelocity / postTimeMax) * (pow(time2, 2) - pow(time1, 2));
    }
  }
}

void SPiiPlusController::assembleFullProfile(int numPoints)
{
  int i;
  unsigned int j;
  int profileIdx;
  //static const char *functionName = "assembleFullProfile";

  /*
   * Assemble the full profile array from the component arrays.
   * The starting point, pAxes_[j]->profilePreDistance_, is not included.
   * The first point of the user-specified profile is ignored because the
   * first time in the user-specified array is not meaningful and the 
   * first position in the user-specified array is the same as the last 
   * position of the acceleration position array.
   */
  profileIdx = 0;
  for (i=0; i<numAccelSegments_; i++)
  {
    fullProfileTimes_[profileIdx] = profileAccelTimes_[i];
    for (j=0; j<profileAxes_.size(); j++)
    {
      pAxes_[j]->fullProfilePositions_[profileIdx] = pAxes_[j]->profileAccelPositions_[i];
    }
    profileIdx++;
  }
  for (i=1; i<numPoints; i++)
  {
    fullProfileTimes_[profileIdx] = profileTimes_[i];
    for (j=0; j<profileAxes_.size(); j++)
    {
      pAxes_[j]->fullProfilePositions_[profileIdx] = pAxes_[j]->profilePositions_[i];
    }
    profileIdx++;
  }
  for (i=0; i<numDecelSegments_; i++)
  {
    fullProfileTimes_[profileIdx] = profileDecelTimes_[i];
    for (j=0; j<profileAxes_.size(); j++)
    {
      pAxes_[j]->fullProfilePositions_[profileIdx] = pAxes_[j]->profileDecelPositions_[i];
    }
    profileIdx++;
  }
  // fullProfileSize_ == profileIdx at this point
  fullProfileSize_ = numAccelSegments_ + (numPoints-1) + numDecelSegments_;
}

void SPiiPlusController::calculateDataCollectionInterval()
{
  int i;
  double time;
  // static const char *functionName = "calculateDataCollectionInterval";
  
  for (i=0; i<fullProfileSize_; i++)
  {
    time += fullProfileTimes_[i];
  }
  
  dataCollectionInterval_ = time / maxProfilePoints_;
}

/** Function to execute a coordinated move of multiple axes. */
asynStatus SPiiPlusController::executeProfile()
{
  // static const char *functionName = "executeProfile";
  epicsEventSignal(profileExecuteEvent_);
  return asynSuccess;
}

/* C Function which runs the profile thread */ 
static void SPiiPlusProfileThreadC(void *pPvt)
{
  SPiiPlusController *pC = (SPiiPlusController*)pPvt;
  pC->profileThread();
}

/* Function which runs in its own thread to execute profiles */ 
void SPiiPlusController::profileThread()
{
  while (true) {
    epicsEventWait(profileExecuteEvent_);
    runProfile();
  }
}

/* Function to run trajectory.  It runs in a dedicated thread, so it's OK to block.
 * It needs to lock and unlock when it accesses class data. */ 
asynStatus SPiiPlusController::runProfile()
{
  int status;
  bool executeOK=true;
  bool aborted=false;
  int startPulses, endPulses;
  //int lastTime;
  int numPoints, numPulses;
  //int numElements;
  int executeStatus;
  //double pulsePeriod;
  double position;
  //double time;
  int i;
  unsigned int j;
  int moveMode;
  char message[MAX_MESSAGE_LEN];
  std::string positions;
  std::stringstream positionStr;
  std::stringstream commandStr;
  std::stringstream cmd;
  SPiiPlusAxis *pAxis;
  int ptExecIdx;
  int ptLoadedIdx;
  int ptFree;
  int ptIdx;
  std::string posData;
  static const char *functionName = "runProfile";
  
  if (profileAxes_.size() == 0)
  {
    strcpy(message, "No axes selected");
    executeOK = false;
    goto done;
  }
  
  asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "%s:%s: axisList = %s\n", driverName, functionName, axesToString(profileAxes_).c_str());
  
  lock();
  getIntegerParam(profileStartPulses_, &startPulses);
  getIntegerParam(profileEndPulses_,   &endPulses);
  getIntegerParam(profileNumPoints_,   &numPoints);
  getIntegerParam(profileNumPulses_,   &numPulses);
  
  sprintf(message, "Selected axes: %s", motorsToString(profileAxes_).c_str()); 
  setStringParam(profileExecuteMessage_, message);
  setIntegerParam(profileExecuteState_, PROFILE_EXECUTE_MOVE_START);
  setIntegerParam(profileExecuteStatus_, PROFILE_STATUS_UNDEFINED);
  
  // Set the readback status to undefined so the user can see that a new read should be done when this move is done executing
  setIntegerParam(profileReadbackStatus_, PROFILE_STATUS_UNDEFINED);
  
  callParamCallbacks();
  unlock();
  
  getIntegerParam(profileMoveMode_, &moveMode);
  
  // move motors to the starting position
  if (moveMode == PROFILE_MOVE_MODE_ABSOLUTE)
  {
    cmd << "PTP/m ";
  }
  else
  {
    cmd << "PTP/mr ";
  }
  for (j=0; j<profileAxes_.size(); j++)
  {
    pAxis = getAxis(profileAxes_[j]);
    
    position = pAxis->profileStartPos_;
    
    if (profileAxes_[j] == profileAxes_.front())
    {
      positionStr << position;
    }
    else 
    {
      positionStr << ',' << position;
    }
  }
  cmd << axesToString(profileAxes_) << ", " << positionStr.str();
  status = writeReadAck(cmd);
  // Should this be done after every command in this method?
  if (status)
  {
    executeOK = false;
    goto done;
  }
  
  // Wait for the motors to get there
  wakeupPoller();
  waitMotors();
  
  if (halted_)
  {
    aborted = true;
    executeOK = false;
    strcpy(message, "Aborted during move to start");
    goto done;
  }
  
  lock();
  setIntegerParam(profileExecuteState_, PROFILE_EXECUTE_EXECUTING);
  callParamCallbacks();
  unlock();
  
  /* configure data recording, which will start when the GO command is issued */
  int axesToRecord;
  if (profileAxes_.size() > 8)
    axesToRecord = 8;
  else
    axesToRecord = profileAxes_.size();
  for (i=0; i<axesToRecord; i++)
  {
    // Zero the data array
    cmd << "FILL(0,DC_DATA_" << (i+1) << ")";
    status = writeReadAck(cmd);
    
    // DC/sw DC_DATA_#,maxProfilePoints_,3,FPOS(a),PE(a),TIME
    if (pAxes_[profileAxes_[i]]->dummy_)
      // use the desired position for dummy axes, since FPOS and PE are always zero
      posData = "APOS";
    else
      // use the feedback position for real motors
      posData = "FPOS";
    cmd << "DC/sw " << profileAxes_[i] << ",DC_DATA_" << (i+1) << "," << maxProfilePoints_ << ",";
    cmd << lround(dataCollectionInterval_ * 1000.0) << "," << posData << "(" << profileAxes_[i] << "),PE(" << profileAxes_[i] << "),TIME";
    status = writeReadAck(cmd);
  }
  
  /*
   *  There is a bug in the controller firmware that prevents synchronized data 
   * collection from starting when a the GO is issued for the PTP/tw move.  If 
   * more than two axes are scanned, data collection must be started *before* 
   * the PTP/tw is sent to the controller, which results in somewhat synchronized 
   * data.  If only one axis is scanned, two GO commands could be sent after 
   * the PTP/tw command, but that isn't implemented.
   */
  // Ugly hack: A GO is needed here to start data collection.
  cmd << "GO " << axesToString(profileAxes_);
  status = writeReadAck(cmd);
  
  // configure pulse output

  // wake up poller
  //wakeupPoller();
  
  /* run the trajectory */
  
  ptLoadedIdx = 0;
  ptExecIdx = 0;
  
  lock();
  setIntegerParam(profileCurrentPoint_, ptExecIdx);
  callParamCallbacks();
  unlock();
  
  // Send the command to start the coordinated motion, but wait for the GO command to move motors
  if (moveMode == PROFILE_MOVE_MODE_ABSOLUTE)
  {
    cmd << "PATH/tw ";
  }
  else
  {
    cmd << "PATH/twr ";
  }
  cmd << axesToString(profileAxes_);
  status = writeReadAck(cmd);
  
  // Fill the point buffer, which can only hold 50 points
  for (ptIdx = 0; ptIdx < MIN(50, fullProfileSize_); ptIdx++)
  {
    // Create and send the point command (should this be ptIdx+1?)
    cmd << "POINT " << axesToString(profileAxes_) << ", " << positionsToString(ptIdx) << ", " << lround(fullProfileTimes_[ptIdx] * 1000.0);
    status = writeReadAck(cmd);
    
    // Increment the counter of points that have been loaded
    ptLoadedIdx++;
  }
  
  if (fullProfileSize_ > 50)
  {
    // Send the GO command
    cmd << "GO " << axesToString(profileAxes_);
    status = writeReadAck(cmd);
    
    while (ptLoadedIdx < fullProfileSize_)
    {
      if (halted_)
      {
        aborted = true;
        executeOK = false;
        status = stopDataCollection();
        strcpy(message, "Aborted during profile move");
        goto done;
      }
      
      // Sleep for a short period of time
      epicsThreadSleep(0.1);
      
      // Query the number of free points in the buffer (the first axis in the vector is the lead axis)
      cmd << "?GSFREE(" << profileAxes_[0] << ")";
      status = writeReadInt(cmd, &ptFree);
      
      // Increment the counter of points that have been executed
      ptExecIdx += ptFree;
      
      // load the rest of the points as needed
      for (ptIdx=ptLoadedIdx; ptIdx<(ptLoadedIdx+ptFree); ptIdx++)
      {
        // Create and send the point command (should this be ptIdx+1?)
        cmd << "POINT " << axesToString(profileAxes_) << ", " << positionsToString(ptIdx) << ", " << lround(fullProfileTimes_[ptIdx] * 1000.0);
        status = writeReadAck(cmd);
      }
      
      // Increment the counter of points that have been loaded
      ptLoadedIdx += ptFree;
      
      lock();
      // Only report the current point of the user-specified array
      if (ptExecIdx > numAccelSegments_)
        setIntegerParam(profileCurrentPoint_, ptExecIdx-numAccelSegments_);
      else
        setIntegerParam(profileCurrentPoint_, 0);
      callParamCallbacks();
      unlock();
    }
    
    // End the point sequence
    cmd << "ENDS " << axesToString(profileAxes_);
    status = writeReadAck(cmd);
  }
  else
  {
    // End the point sequence
    cmd << "ENDS " << axesToString(profileAxes_);
    status = writeReadAck(cmd);
    
    // Send the GO command
    cmd << "GO " << axesToString(profileAxes_);
    status = writeReadAck(cmd);
  }
  
  // Wait for the remaining points to be executed
  while (ptExecIdx < fullProfileSize_)
  {
    if (halted_)
    {
      aborted = true;
      executeOK = false;
      status = stopDataCollection();
      strcpy(message, "Aborted during profile move");
      goto done;
    }
    
    // Sleep for a short period of time
    epicsThreadSleep(0.1);
    
    // Query the number of free points in the buffer
    cmd << "?GSFREE(" << profileAxes_[0] << ")";
    status = writeReadInt(cmd, &ptFree);
    
    // Update the number of points that have been executed
    ptExecIdx = fullProfileSize_ - 50 + ptFree;
    
    lock();
    // Stop updating current point when numPoints is reached
    if (ptExecIdx < numAccelSegments_)
      // This only gets executed if the user-specified profile has very few points in it
      setIntegerParam(profileCurrentPoint_, 0);
    else if ((ptExecIdx >= numAccelSegments_) && (ptExecIdx < numAccelSegments_+numPoints))
      setIntegerParam(profileCurrentPoint_, ptExecIdx-numAccelSegments_);
    else
      setIntegerParam(profileCurrentPoint_, numPoints);
    callParamCallbacks();
    unlock();
  }
  
  // Confirm that the motors are done moving?
  
  lock();
  setIntegerParam(profileExecuteState_, PROFILE_EXECUTE_FLYBACK);
  callParamCallbacks();
  unlock();
  
  /* move motors to the end position */
  positionStr.str("");
  positionStr.clear();
  if (moveMode == PROFILE_MOVE_MODE_ABSOLUTE)
  {
    cmd << "PTP/m ";
  }
  else
  {
    cmd << "PTP/mr ";
  }
  // Create the comma-separated list of final positions
  for (j=0; j<profileAxes_.size(); j++)
  {
    pAxis = getAxis(profileAxes_[j]);
    
    position = pAxis->profileFlybackPos_;
    
    if (profileAxes_[j] == profileAxes_.front())
    {
      positionStr << position;
    }
    else 
    {
      positionStr << ',' << position;
    }
  }
  // Send the group move command
  cmd << axesToString(profileAxes_) << ", " << positionStr.str();
  status = writeReadAck(cmd);

  // Wait for the motors to get there
  wakeupPoller();
  waitMotors();
  
  if (halted_)
  {
    aborted = true;
    executeOK = false;
    strcpy(message, "Aborted during flyback");
    goto done;
  }
  
  done:
  lock();
  if (executeOK)    executeStatus = PROFILE_STATUS_SUCCESS;
  else if (aborted) executeStatus = PROFILE_STATUS_ABORT;
  else              executeStatus = PROFILE_STATUS_FAILURE;
  setIntegerParam(profileExecuteStatus_, executeStatus);
  setStringParam(profileExecuteMessage_, message);
  if (!executeOK) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: %s\n",
              driverName, functionName, message);
  }
  /* Clear execute command.  This is a "busy" record, don't want to do this until build is complete. */
  setIntegerParam(profileExecute_, 0);
  setIntegerParam(profileExecuteState_, PROFILE_EXECUTE_DONE);
  callParamCallbacks();
  halted_ = false;
  unlock();
  return executeOK ? asynSuccess : asynError; 
}

asynStatus SPiiPlusController::waitMotors()
{
  unsigned int j;
  SPiiPlusAxis* pAxis;
  int moving;
  static const char *functionName = "waitMotors";
  
  while (1) {
    epicsThreadSleep(0.1);
    
    // assume no motors are moving
    moving = 0;
    for (j=0; j<profileAxes_.size(); j++)
    {
      pAxis = getAxis(profileAxes_[j]);
      moving |= pAxis->moving_;
    }
    if (moving == 0) break;
  }
  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s:%s: motors are done moving\n", driverName, functionName);
  return asynSuccess;
}

asynStatus SPiiPlusController::stopDataCollection()
{
  asynStatus status;
  std::stringstream cmd;
  int i;
  int axesToRecord;
  // static const char *functionName = "stopDataCollection";  
  
  if (profileAxes_.size() > 8)
    axesToRecord = 8;
  else
    axesToRecord = profileAxes_.size();
  for (i=0; i<axesToRecord; i++)
  {
    cmd << "STOPDC/s " << profileAxes_[i];
    status = writeReadAck(cmd);
  }
  
  return status;
}

/** Function to abort a profile. */
asynStatus SPiiPlusController::abortProfile()
{
  asynStatus status;
  std::stringstream cmd;
  int executeState;
  // static const char *functionName = "abortProfile";
  
  getIntegerParam(profileExecuteState_,   &executeState);
    
  if (executeState != PROFILE_EXECUTE_DONE)
  {
    cmd << "HALT " << axesToString(profileAxes_);
    status = writeReadAck(cmd);
    
    halted_ = true;
  }
  
  return status;
}

asynStatus SPiiPlusController::readbackProfile()
{
  char message[MAX_MESSAGE_LEN];
  bool readbackOK=true;
  char* buffer=NULL;
  int readbackStatus;
  int status;
  int i; 
  unsigned int j;
  std::stringstream cmd;
  char var[MAX_MESSAGE_LEN];
  int bytesPerDim;
  SPiiPlusAxis* pAxis;
  static const char *functionName = "readbackProfile";

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
            "%s:%s: entry\n",
            driverName, functionName);

  strcpy(message, "");
  setStringParam(profileReadbackMessage_, message);
  setIntegerParam(profileReadbackState_, PROFILE_READBACK_BUSY);
  setIntegerParam(profileReadbackStatus_, PROFILE_STATUS_UNDEFINED);
  callParamCallbacks();
  
  if (profileAxes_.size() == 0)
  {
    strcpy(message, "No axes selected");
    readbackOK = false;
    goto done;
  }
  
  asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "%s:%s: axisList = %s\n", driverName, functionName, axesToString(profileAxes_).c_str());
  sprintf(message, "Selected axes: %s", motorsToString(profileAxes_).c_str()); 
  setStringParam(profileReadbackMessage_, message);
  callParamCallbacks();
  
  /* Erase the readback and error arrays */
  for (i=0; i<numAxes_; i++) {
    memset(pAxes_[i]->profileReadbacks_,       0, maxProfilePoints_*sizeof(double));
    memset(pAxes_[i]->profileFollowingErrors_, 0, maxProfilePoints_*sizeof(double));
  }
  
  buffer = (char *)calloc(MAX_BINARY_READ_LEN, sizeof(char));
  
  for (j=0; j<profileAxes_.size(); j++)
  {
    pAxis = getAxis(j);
    
    sprintf(var, "DC_DATA_%i", j+1);
    status = getDoubleArray(buffer, var, 0, 2, 0, (maxProfilePoints_-1));
    if (status != asynSuccess)
    {
      readbackOK = false;
      goto done;
    }
    
    bytesPerDim = maxProfilePoints_ * sizeof(double);
    // Position
    memcpy(pAxis->profileReadbacks_, buffer, bytesPerDim);
    // Postion error
    memcpy(pAxis->profileFollowingErrors_, buffer+bytesPerDim, bytesPerDim);
    // Time
    //memcpy(pAxis->profileTimes_, buffer+bytesPerDim*2, bytesPerDim);
  }
  
  done:
  if (buffer) free(buffer);
  setIntegerParam(profileNumReadbacks_, maxProfilePoints_);
  /* Convert from controller to user units and post the arrays */
  for (i=0; i<numAxes_; i++) {
    pAxes_[i]->readbackProfile();
  }
  readbackStatus = readbackOK ?  PROFILE_STATUS_SUCCESS : PROFILE_STATUS_FAILURE;
  setIntegerParam(profileReadbackStatus_, readbackStatus);
  setStringParam(profileReadbackMessage_, message);
  if (!readbackOK) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: %s\n",
              driverName, functionName, message);
  }
  /* Clear readback command.  This is a "busy" record, don't want to do this until readback is complete. */
  setIntegerParam(profileReadback_, 0);
  setIntegerParam(profileReadbackState_, PROFILE_READBACK_DONE);
  callParamCallbacks();
  return status ? asynError : asynSuccess; 
  
  return asynSuccess;
}

asynStatus SPiiPlusController::test()
{
  asynStatus status;
  char* buffer=NULL;
  
  static const char *functionName = "test";
  
  asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s:%s: calling test function\n", driverName, functionName);
  
  buffer = (char *)calloc(MAX_BINARY_READ_LEN, sizeof(char));
  
  status = getDoubleArray(buffer, "DC_DATA_1", 0, 2, 0, (maxProfilePoints_-1));
  
  free(buffer);
  
  return status;
}


/** Reports on status of the driver
  * \param[in] fp The file pointer on which report information will be written
  * \param[in] level The level of report detail desired
  *
  * If details > 0 then information is printed about each axis.
  * After printing controller-specific information it calls asynMotorController::report()
  */
void SPiiPlusController::report(FILE *fp, int level)
{
  fprintf(fp, "====================================\n");
  fprintf(fp, "SPiiPlus motor driver:\n");
  fprintf(fp, "    asyn port: %s\n", this->portName);
  fprintf(fp, "    num axes: %i\n", numAxes_);
  fprintf(fp, "    moving poll period: %lf\n", movingPollPeriod_);
  fprintf(fp, "    idle poll period: %lf\n", idlePollPeriod_);
  fprintf(fp, "\n");
  
  // Print more axis detail if level = 1
  // Print all the asyn parameters if level > 1
  if (level > 0)
    level -= 1;
  
  // Call the base class method
  asynMotorController::report(fp, level-1);
  fprintf(fp, "====================================\n");
}

static void AcsMotionConfig(const char* acs_port, const char* asyn_port, int num_axes, double moving_rate, double idle_rate)
{
	new SPiiPlusController(acs_port, asyn_port, num_axes, moving_rate, idle_rate);
}


extern "C"
{

asynStatus SPiiPlusCreateProfile(const char *SPiiPlusName,         /* specify which controller by port name */
                            int maxPoints)               /* maximum number of profile points */
{
  SPiiPlusController *pC;
  static const char *functionName = "SPiiPlusCreateProfile";

  pC = (SPiiPlusController*) findAsynPortDriver(SPiiPlusName);
  if (!pC) {
    printf("%s:%s: Error port %s not found\n",
           driverName, functionName, SPiiPlusName);
    return asynError;
  }
  pC->lock();
  pC->initializeProfile(maxPoints);
  pC->unlock();
  return asynSuccess;
}

// Profile Setup arguments
static const iocshArg SPiiPlusCreateProfileArg0 = {"ACS port name", iocshArgString};
static const iocshArg SPiiPlusCreateProfileArg1 = {"Max points", iocshArgInt};

static const iocshArg * const SPiiPlusCreateProfileArgs[2] = {&SPiiPlusCreateProfileArg0, &SPiiPlusCreateProfileArg1};

static const iocshFuncDef configSPiiPlusProfile = {"SPiiPlusCreateProfile", 2, SPiiPlusCreateProfileArgs};

static void configSPiiPlusProfileCallFunc(const iocshArgBuf *args)
{
    SPiiPlusCreateProfile(args[0].sval, args[1].ival);
}

// ACS Setup arguments
static const iocshArg configArg0 = {"ACS port name", iocshArgString};
static const iocshArg configArg1 = {"asyn port name", iocshArgString};
static const iocshArg configArg2 = {"num axes", iocshArgInt};
static const iocshArg configArg3 = {"Moving polling rate", iocshArgDouble};
static const iocshArg configArg4 = {"Idle polling rate", iocshArgDouble};

static const iocshArg * const AcsMotionConfigArgs[5] = {&configArg0, &configArg1, &configArg2, &configArg3, &configArg4};

static const iocshFuncDef configAcsMotion = {"AcsMotionConfig", 5, AcsMotionConfigArgs};

static void AcsMotionCallFunc(const iocshArgBuf *args)
{
    AcsMotionConfig(args[0].sval, args[1].sval, args[2].ival, args[3].dval, args[4].dval);
}

// Register all the commands users can call from the command line
static void AcsMotionRegister(void)
{
	iocshRegister(&configAcsMotion, AcsMotionCallFunc);
	iocshRegister(&configSPiiPlusProfile, configSPiiPlusProfileCallFunc);
}

epicsExportRegistrar(AcsMotionRegister);

}
