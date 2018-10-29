/*	opendatacon
*
*	Copyright (c) 2018:
*
*		DCrip3fJguWgVCLrZFfA7sIGgvx1Ou3fHfCxnrz4svAi
*		yxeOtDhDCXf1Z4ApgXvX5ahqQmzRfJ2DoX8S05SqHA==
*
*	Licensed under the Apache License, Version 2.0 (the "License");
*	you may not use this file except in compliance with the License.
*	You may obtain a copy of the License at
*
*		http://www.apache.org/licenses/LICENSE-2.0
*
*	Unless required by applicable law or agreed to in writing, software
*	distributed under the License is distributed on an "AS IS" BASIS,
*	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*	See the License for the specific language governing permissions and
*	limitations under the License.
*/
/*
* CBOutStationPortHandlers.cpp
*
*  Created on: 12/09/2018
*      Author: Scott Ellis <scott.ellis@novatex.com.au>
*/

/* The out station port is connected to the Overall System Scada master, so the master thinks it is talking to an outstation.
 This code then fires off events to the connector, which the connected master port(s) (of some type DNP3/ModBus/CB) will turn back into scada commands and send out to the "real" Outstation.
 So it makes sense to connect the SIM (which generates data) to a DNP3 Outstation which will feed the data back to the SCADA master.
 So an Event to an outstation will be data that needs to be sent up to the scada master.
 An event from an outstation will be a master control signal to turn something on or off.
*/
#include <iostream>
#include <future>
#include <regex>
#include <chrono>

#include "CB.h"
#include "CBUtility.h"
#include "CBOutstationPort.h"


void CBOutstationPort::ProcessCBMessage(CBMessage_t &CompleteCBMessage)
{
	// We know that the address matches in order to get here, and that we are in the correct INSTANCE of this class.
	assert(CompleteCBMessage.size() != 0);

	size_t ExpectedMessageSize = 1; // Only set in switch statement if not 1

	CBBlockData &Header = CompleteCBMessage[0];
	// Now based on the PendingCommand Function, take action. Some of these are responses from - not commands to an OutStation.

	bool NotImplemented = false;

	// All are included to allow better error reporting.
	// The Python simulator only implements the commands below, which will be our first target:
	//'0': 'scan data',
	//	'1' : 'execute',
	//	'2' : 'trip/close',
	//	'4' : 'trip/close',
	//	'9' : 'master station request'
	switch (Header.GetFunctionCode())
	{
		case FUNC_SCAN_DATA:
			ScanRequest(Header); // Fn - 0
			break;

		case FUNC_EXECUTE_COMMAND:
			// The other functions below will setup what needs to be executed, has to be executed within a certain time...
			ExecuteCommand(Header);
			break;

		case FUNC_TRIP:
			// Binary Control
			FuncTripClose(Header, PendingCommandType::CommandType::Trip);
			break;

		case FUNC_SETPOINT_A:
			FuncSetAB(Header, PendingCommandType::CommandType::SetA);
			break;

		case FUNC_CLOSE:
			// Binary Control
			FuncTripClose(Header, PendingCommandType::CommandType::Close);
			break;

		case FUNC_SETPOINT_B:
			FuncSetAB(Header, PendingCommandType::CommandType::SetB);
			break;

		case FUNC_RESET: // Not in documentation....
			NotImplemented = true;
			break;

		case FUNC_MASTER_STATION_REQUEST:
			// Has many sub messages, determined by Group Number
			// This is the only code that has extra Master to RTU blocks
			FuncMasterStationRequest(Header, CompleteCBMessage);
			break;

		case FUNC_SEND_NEW_SOE:
			NotImplemented = true;
			break;
		case FUNC_REPEAT_SOE:
			// Resend the last SOE message
			NotImplemented = true;
			break;
		case FUNC_UNIT_RAISE_LOWER:
			NotImplemented = true;
			break;
		case FUNC_FREEZE_AND_SCAN_ACC:
			// Warm Restart (from python sim)??
			NotImplemented = true;
			break;
		case FUNC_FREEZE_SCAN_AND_RESET_ACC:
			// Cold restart (from python sim)??
			NotImplemented = true;
			break;

		default:
			LOGERROR("Unknown PendingCommand Function - " + std::to_string(Header.GetFunctionCode()) + " On Station Address - " + std::to_string(Header.GetStationAddress()));
			break;
	}
	if (NotImplemented == true)
	{
		LOGERROR("PendingCommand Function NOT Implemented - " + std::to_string(Header.GetFunctionCode()) + " On Station Address - " + std::to_string(Header.GetStationAddress()));
	}
}

#pragma region RESPONSE METHODS

void CBOutstationPort::ScanRequest(CBBlockData &Header)
{
	LOGDEBUG("OS - ScanRequest - Fn0");

	// Assemble the block values A and B in order ready to be placed into the response message.
	std::vector<uint16_t> BlockValues;

	// Use the group definition to assemble the scan data
	BuildScanRequestResponseData(Header.GetGroup(), BlockValues);

	// Now assemble and return the required response..
	CBMessage_t ResponseCBMessage;
	uint32_t index = 0;
	uint16_t FirstBlockBValue = 0x000; // Default

	if (BlockValues.size() > 0)
	{
		FirstBlockBValue = BlockValues[index++];
	}

	// The first block is mostly an echo of the request, except that the B field contains data.
	auto firstblock = CBBlockData(Header.GetStationAddress(), Header.GetGroup(), Header.GetFunctionCode(), FirstBlockBValue, BlockValues.size() < 2);
	ResponseCBMessage.push_back(firstblock);

	// index will be 1 when we get to here..
	while (index < BlockValues.size())
	{
		uint16_t A = BlockValues[index++];

		// If there is no B value, default to zero.
		uint16_t B = 0x00;
		if (index < BlockValues.size())
		{
			B = BlockValues[index++]; // This should always be triggered as our BuildScanResponseData should give us a full message. We handle the missing case anyway!
		}
		auto block = CBBlockData(A, B, index >= BlockValues.size()); // Just some data, end of message is true if we have no more data.
		ResponseCBMessage.push_back(block);
	}

	SendCBMessage(ResponseCBMessage);
}

void CBOutstationPort::BuildScanRequestResponseData(uint8_t Group, std::vector<uint16_t> &BlockValues)
{
	// We now have to collect all the current values for this group.
	// Search for the group and payload location, and if we have data process it. We have to search 3 lists and the RST table to get what we need
	// This will always return a complete set of blocks, i.e. there will always be a last B value.

	uint8_t MaxBlockNum = 1; // If the following fails, we just respond with an empty single block.

	MyPointConf->PointTable.GetMaxPayload(Group, MaxBlockNum);

	// Block 1B will always need a value. 1A is always emtpy - used for group/station/function
	PayloadLocationType payloadlocationA(1, PayloadABType::PositionB);
	BlockValues.push_back(GetPayload(Group, payloadlocationA)); // Returns 0 if payload value cannot be found and logs a message

	for (uint8_t blocknumber = 2; blocknumber <= MaxBlockNum; blocknumber++)
	{
		PayloadLocationType payloadlocationA(blocknumber, PayloadABType::PositionA );
		BlockValues.push_back(GetPayload(Group, payloadlocationA));

		PayloadLocationType payloadlocationB(blocknumber, PayloadABType::PositionB);
		BlockValues.push_back(GetPayload(Group, payloadlocationB));
	}
}

uint16_t CBOutstationPort::GetPayload(uint8_t &Group, PayloadLocationType &payloadlocation)
{
	bool FoundMatch = false;
	uint16_t Payload = 0;

	MyPointConf->PointTable.ForEachMatchingAnalogPoint(Group, payloadlocation, [&](CBAnalogCounterPoint &pt)
		{
			// We have a matching point - there may be 2, set a flag to indicate we have a match, and set our bits in the output.
			uint8_t ch = pt.GetChannel();
			if ((pt.GetPointType() == ANA6) && (ch == 1))
			{
			      Payload |= ShiftLeftResult16Bits(pt.GetAnalog(), 6);
			}
			else
				Payload |= pt.GetAnalog();

			FoundMatch = true;
		});

	if (!FoundMatch)
	{
		MyPointConf->PointTable.ForEachMatchingCounterPoint(Group, payloadlocation, [&](CBAnalogCounterPoint &pt)
			{
				// We have a matching point - there will be only 1!!, set a flag to indicate we have a match, and set our bit in the output.
				Payload = pt.GetAnalog();
				FoundMatch = true;
			});
	}
	if (!FoundMatch)
	{
		MyPointConf->PointTable.ForEachMatchingBinaryPoint(Group, payloadlocation, [&](CBBinaryPoint &pt)
			{
				// We have a matching point, set a flag to indicate we have a match, and set our bit in the output.
				uint8_t ch = pt.GetChannel();
				if (pt.GetPointType() == DIG)
				{
				      if (pt.GetBinary() == 1)
				      {
				                                                          // ch 1 to 12
				            Payload |= ShiftLeftResult16Bits(1, 12 - ch); // ch 1 TO 12, Just have to OR, we know it was zero initially.
					}
				}
				else if ((pt.GetPointType() == MCA) || (pt.GetPointType() == MCB) || (pt.GetPointType() == MCC))
				{
				// These types are supposed to occupy 2 consecutive 12 bit blocks, channels 1 to 12.
				// We can handle them as two separately defined blocks (but consecutive) channels 1 to 6.
				//TODO: Will check for two consecutive MC blocks in the conf file loading??
				// MCA - The change bit is set when the input changes from open to closed (1-->0). The status bit is 0 when the contact is CLOSED.
				// MCB - The change bit is set when the input changes from closed to open (0-->1). The status bit is 0 when the contact is OPEN.
				// MCC - The change bit is set when the input has gone through more than one change of state. The status bit is 0 when the contact is OPEN.
				// We dont think about open or closed, we will just be getting a value of 1 or 0 from the real outstation, or a simulator. So we dont do inversion or anything like that.
				// We do need to track the types of transision, and the point has a special field to do this.

				      // Set our bit and MC changed flag in the output. Data bit is first then change bit So bit 11 = data, bit 10 = change in 12 bit word - 11 highest bit.
				      uint8_t result;
				      bool MCS;
				      pt.GetBinaryAndMCFlagWithFlagReset(result, MCS);
				      if (result == 1)
				      {
				                                                                  // ch 1 to 6
				            Payload |= ShiftLeftResult16Bits(1, 11 - (ch-1) * 2); // CH 1 to 6!!
					}
				      if (MCS)
				      {
				// ch 1 to 6
				            Payload |= ShiftLeftResult16Bits(1, 11 - ((ch-1) * 2 - 1));
					}
				}
				else
				{
				      LOGERROR("Unhandled Binary Point type - no valid value returned");
				}
				FoundMatch = true;
			});
	}
	if (!FoundMatch)
	{
		// See if it is a StatusByte we need to provide - there is only one status byte, but it could be requested in several groups.
		MyPointConf->PointTable.ForEachMatchingStatusByte(Group, payloadlocation, [&](void)
			{
				// We have a matching status byte, set a flag to indicate we have a match.
				LOGDEBUG("Got a Status Byte request at : " + std::to_string(Group) + " - " + payloadlocation.to_string());
				Payload = 0x555; //TODO: Have to populate the status byte
				FoundMatch = true;
			});
	}
	if (!FoundMatch)
	{
		LOGDEBUG("Failed to find a payload for: " + payloadlocation.to_string() + " Setting to zero");
	}
	return Payload;
}


void CBOutstationPort::FuncTripClose(CBBlockData &Header, PendingCommandType::CommandType pCommand)
{
	// Sets up data to be executed when we get the execute command...
	// So we dont trigger any ODC events at this point, only when the execute occurs.
	std::string cmd = "Trip";
	if (pCommand == PendingCommandType::CommandType::Close) cmd = "Close";

	LOGDEBUG("OS - {} PendingCommand - Fn2/4",cmd);

	uint8_t group = Header.GetGroup();
	PendingCommands[group].Data = Header.GetB();

	// Can/must only be 1 bit set, check this.
	if (GetBitsSet(PendingCommands[group].Data,12) == 1)
	{
		// Check that this is actually a valid CONTROL point.
		uint8_t Channel = 1 + GetSetBit(PendingCommands[group].Data, 12);
		size_t ODCIndex;
		if (!MyPointConf->PointTable.GetBinaryControlODCIndexUsingCBIndex(group, Channel, ODCIndex))
		{
			PendingCommands[group].Command = PendingCommandType::CommandType::None;
			LOGDEBUG("FuncTripClose - Could not find an ODC BinaryControl to match Group {}, channel {}", Header.GetGroup(), Channel);
			return;
		}

		PendingCommands[group].Command = pCommand;
		PendingCommands[group].ExpiryTime = CBNow() + PendingCommands[group].CommandValidTimemsec;

		LOGDEBUG("OS - Got a valid {} PendingCommand, Data {}",cmd, PendingCommands[group].Data);

		auto firstblock = CBBlockData(Header.GetStationAddress(), Header.GetGroup(), Header.GetFunctionCode(), PendingCommands[group].Data, true);

		// Now assemble and return the required response..
		CBMessage_t ResponseCBMessage;
		ResponseCBMessage.push_back(firstblock);

		SendCBMessage(ResponseCBMessage);
	}
	else
	{
		PendingCommands[group].Command = PendingCommandType::CommandType::None;

		// Error - dont reply..
		LOGERROR("OS - More than one or no bit set in a {} PendingCommand - Not responding to master",cmd);
	}
}

void CBOutstationPort::FuncSetAB(CBBlockData &Header, PendingCommandType::CommandType pCommand)
{
	// Sets up data to be executed when we get the execute command...
	// So we dont trigger any ODC events at this point, only when the execute occurs.
	std::string cmd = "SetA";
	uint8_t Channel = 1;

	if (pCommand == PendingCommandType::CommandType::SetB)
	{
		Channel = 2;
		cmd = "SetB";
	}

	LOGDEBUG("OS - {} PendingCommand - Fn3/5", cmd);
	uint8_t group = Header.GetGroup();

	// Also now check that this is actually a valid CONTROL point.
	size_t ODCIndex;
	if (!MyPointConf->PointTable.GetAnalogControlODCIndexUsingCBIndex(group, Channel, ODCIndex))
	{
		PendingCommands[group].Command = PendingCommandType::CommandType::None;
		LOGDEBUG("FuncSetAB - Could not find an ODC AnalogControl to match Group {}, channel {}", group, Channel);
		return;
	}

	PendingCommands[group].Command = pCommand;
	PendingCommands[group].ExpiryTime = CBNow() + PendingCommands[group].CommandValidTimemsec;
	PendingCommands[group].Data = Header.GetB();

	LOGDEBUG("OS - Got a valid {} PendingCommand, Data {}", cmd, PendingCommands[group].Data);

	auto firstblock = CBBlockData(Header.GetStationAddress(), Header.GetGroup(), Header.GetFunctionCode(), PendingCommands[group].Data, true);

	// Now assemble and return the required response..
	CBMessage_t ResponseCBMessage;
	ResponseCBMessage.push_back(firstblock);

	SendCBMessage(ResponseCBMessage);
}

void CBOutstationPort::ExecuteCommand(CBBlockData &Header)
{
	// Now if there is a command to be executed - do so
	LOGDEBUG("OS - ExecuteCommand - Fn1");

	// Find a matching PendingCommand (by Group)
	PendingCommandType &PendingCommand = PendingCommands[Header.GetGroup()];

	if (CBNow() > PendingCommand.ExpiryTime)
	{
		LOGDEBUG("Received an Execute Command, but the current command had expired");
		//TODO: Should we reply in some way?
		return;
	}

	switch (PendingCommand.Command)
	{
		case PendingCommandType::CommandType::None:
			LOGDEBUG("Received an Execute Command, but there is no current command");
			break;

		case PendingCommandType::CommandType::Trip:
		{
			LOGDEBUG("Received an Execute Command, Trip");
			uint8_t SetBit = GetSetBit(PendingCommand.Data, 12);
			bool point_on = false; // Trip is OFF??

			ExecuteBinaryControl(Header.GetGroup(), SetBit+1, point_on);
		}
		break;

		case PendingCommandType::CommandType::Close:
		{
			LOGDEBUG("Received an Execute Command, Close");
			uint8_t SetBit = GetSetBit(PendingCommand.Data, 12);
			bool point_on = true; // Trip is OFF??

			ExecuteBinaryControl(Header.GetGroup(), SetBit+1, point_on);
		}
		break;

		case PendingCommandType::CommandType::SetA:
		{
			LOGDEBUG("Received an Execute Command, SetA");
			ExecuteAnalogControl(Header.GetGroup(), 1, PendingCommand.Data);
		}
		break;
		case PendingCommandType::CommandType::SetB:
		{
			LOGDEBUG("Received an Execute Command, SetB");
			ExecuteAnalogControl(Header.GetGroup(), 2, PendingCommand.Data);
		}
		break;
	}

	PendingCommand.Command = PendingCommandType::CommandType::None;

	auto firstblock = CBBlockData(Header.GetStationAddress(), Header.GetGroup(), Header.GetFunctionCode(), 0, true);
	CBMessage_t ResponseCBMessage;
	ResponseCBMessage.push_back(firstblock);
	SendCBMessage(ResponseCBMessage);
}

void CBOutstationPort::ExecuteBinaryControl(uint8_t group, int channel, bool point_on)
{
	size_t ODCIndex = 0;

	if (!MyPointConf->PointTable.GetBinaryControlODCIndexUsingCBIndex(group, channel, ODCIndex))
	{
		LOGDEBUG("Could not find an ODC BinaryControl to match Group {}, channel {}", group, channel);
		return;
	}

	// Set our output value. Only really used for testing
	MyPointConf->PointTable.SetBinaryControlValueUsingODCIndex(ODCIndex, point_on, CBNow());

	EventTypePayload<EventType::ControlRelayOutputBlock>::type val;
	val.functionCode = point_on ? ControlCode::LATCH_ON : ControlCode::LATCH_OFF;

	auto event = std::make_shared<EventInfo>(EventType::ControlRelayOutputBlock, ODCIndex, Name);
	event->SetPayload<EventType::ControlRelayOutputBlock>(std::move(val));

	bool waitforresult = !MyPointConf->StandAloneOutstation;

	bool success = (Perform(event, waitforresult) == odc::CommandStatus::SUCCESS); // If no subscribers will return quickly.
}
void CBOutstationPort::ExecuteAnalogControl(uint8_t group, int channel, uint16_t data)
{
	// The Setbit+1 == channel
	size_t ODCIndex = 0;

	if (!MyPointConf->PointTable.GetAnalogControlODCIndexUsingCBIndex(group, channel, ODCIndex))
	{
		LOGDEBUG("Could not find an ODC AnalogControl to match Group {}, channel {}", group, channel);
		return;
	}

	// Set our output value. Only really used for testing
	MyPointConf->PointTable.SetAnalogControlValueUsingODCIndex(ODCIndex, data, CBNow());

	EventTypePayload<EventType::AnalogOutputInt16>::type val;
	val.first = data;

	auto event = std::make_shared<EventInfo>(EventType::AnalogOutputInt16, ODCIndex, Name);
	event->SetPayload<EventType::AnalogOutputInt16>(std::move(val));

	bool waitforresult = !MyPointConf->StandAloneOutstation;

	bool success = (Perform(event, waitforresult) == odc::CommandStatus::SUCCESS); // If no subscribers will return quickly.
}
void CBOutstationPort::FuncMasterStationRequest(CBBlockData & Header, CBMessage_t & CompleteCBMessage)
{
	LOGDEBUG("OS - MasterStationRequest - Fn9 - Code {}", Header.GetGroup());
	// The purpose of this function is determined by Group Number
	switch (Header.GetGroup())
	{
		case MASTER_SUB_FUNC_0_NOTUSED:
		case MASTER_SUB_FUNC_SPARE1:
		case MASTER_SUB_FUNC_SPARE2:
		case MASTER_SUB_FUNC_SPARE3:
			// Not used, we do not respond;
			LOGERROR("Received Unused Master Command Function - {} ",Header.GetGroup());
			break;

		case MASTER_SUB_FUNC_TESTRAM:
		case MASTER_SUB_FUNC_TESTPROM:
		case MASTER_SUB_FUNC_TESTEPROM:
		case MASTER_SUB_FUNC_TESTIO:

			// We respond as if everything is OK - we just send back what we got. One block only.
			EchoReceivedHeaderToMaster(Header);
			LOGDEBUG("Received TEST Master Command Function - {}, no action, but we reply", Header.GetGroup());
			break;

		case MASTER_SUB_FUNC_SEND_TIME_UPDATES:
		{
			// This is the only code that has extra Master to RTU blocks
			uint8_t DataIndex = Header.GetB() >> 8;
			uint16_t Data = Header.GetB() & 0x1FF;
			if (DataIndex == 0)
			{
				ProcessUpdateTimeRequest(CompleteCBMessage);
			}
			else
			{
				LOGERROR("Received Illegal MASTER_SUB_FUNC_SEND_TIME_UPDATES DataIndex value - {} ", DataIndex);
			}
		}
		break;

		case MASTER_SUB_FUNC_RETRIEVE_REMOTE_STATUS_WORD:
			EchoReceivedHeaderToMaster(Header);
			LOGDEBUG("Received Get Remote Status Master Command Function - {}, no action, but we reply", Header.GetGroup());
			break;

		case MASTER_SUB_FUNC_RETREIVE_INPUT_CIRCUIT_DATA:
			LOGERROR("Received Input Circuit Data Master Command Function - {}, not implemented", Header.GetGroup());
			break;

		case MASTER_SUB_FUNC_TIME_CORRECTION_FACTOR_ESTABLISHMENT: // Also send Comms Stats, 2nd option
			EchoReceivedHeaderToMaster(Header);
			LOGDEBUG("Received Time Correction Factor Master Command Function - {}, no action, but we reply", Header.GetGroup());
			break;

		case MASTER_SUB_FUNC_REPEAT_PREVIOUS_TRANSMISSION:
			LOGDEBUG("Resending Last Message as a Repeat Last Transmission - {}", CBMessageAsString(LastSentCBMessage));
			ResendLastCBMessage();
			break;

		case MASTER_SUB_FUNC_SET_LOOPBACKS:
			LOGERROR("Received Input Circuit Data Master Command Function - {}, not implemented", Header.GetGroup());
			break;

		case MASTER_SUB_FUNC_RESET_RTU_WARM:
			LOGERROR("Received Warm Restart Master Command Function - {}, not implemented", Header.GetGroup());
			break;

		case MASTER_SUB_FUNC_RESET_RTU_COLD:
			LOGERROR("Received Cold Restart Master Command Function - {}, not implemented", Header.GetGroup());
			break;

		default:
			LOGERROR("Unknown PendingCommand Function - " + std::to_string(Header.GetFunctionCode()) + " On Station Address - " + std::to_string(Header.GetStationAddress()));
			break;
	}
}

// We do not update the time, just send back our current time - assume UTC time of day in milliseconds since 1970 (CBTime()).
void CBOutstationPort::ProcessUpdateTimeRequest(CBMessage_t & CompleteCBMessage)
{
	CBMessage_t ResponseCBMessage;

	BuildUpdateTimeMessage(CompleteCBMessage[0].GetStationAddress(), CBNow(), ResponseCBMessage);

	SendCBMessage(ResponseCBMessage);
}

void CBOutstationPort::EchoReceivedHeaderToMaster(CBBlockData & Header)
{
	auto firstblock = CBBlockData(Header.GetStationAddress(), Header.GetGroup(), Header.GetFunctionCode(), 0, true);
	CBMessage_t ResponseCBMessage;
	ResponseCBMessage.push_back(firstblock);
	SendCBMessage(ResponseCBMessage);
}
#pragma endregion


#pragma region Worker Methods

// This method is passed to the SystemFlags variable to do the necessary calculation
// Access through SystemFlags.GetDigitalChangedFlag()
bool CBOutstationPort::DigitalChangedFlagCalculationMethod(void)
{
	// Return true if there is unsent digital changes
	return (CountBinaryBlocksWithChanges() > 0);
}
// This method is passed to the SystemFlags variable to do the necessary calculation
// Access through SystemFlags.GetTimeTaggedDataAvailableFlag()
bool CBOutstationPort::TimeTaggedDataAvailableFlagCalculationMethod(void)
{
	return false; //TODO: MyPointConf->PointTable.TimeTaggedDataAvailable();
}
void CBOutstationPort::MarkAllBinaryPointsAsChanged()
{
	MyPointConf->PointTable.ForEachBinaryPoint([&](CBBinaryPoint &pt)
		{
			pt.SetChangedFlag();
		});
}


// Scan all binary/digital blocks for changes - used to determine what response we need to send
// We return the total number of changed blocks we assume every block supports time tagging
// If SendEverything is true,
uint8_t CBOutstationPort::CountBinaryBlocksWithChanges()
{
	uint8_t changedblocks = 0;
	int lastblock = -1; // Non valid value

	// The map is sorted, so when iterating, we are working to a specific order. We can have up to 16 points in a block only one changing will trigger a send.
	MyPointConf->PointTable.ForEachBinaryPoint([&](CBBinaryPoint &pt)
		{
			if (pt.GetChangedFlag())
			{
			// Multiple bits can be changed in the block, but only the first one is required to trigger a send of the block.
			      if (lastblock != pt.GetGroup())
			      {
			            lastblock = pt.GetGroup();
			            changedblocks++;
				}
			}
		});

	return changedblocks;
}
#pragma endregion