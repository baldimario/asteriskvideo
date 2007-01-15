#include "H324MAL2.h"
#include "crc8.h"

/****************** Receiver **************/
H223AL2Receiver::H223AL2Receiver(int useSequenceNumbers)
{
	//Set sn parameter
	useSN = useSequenceNumbers;
}

H223AL2Receiver::~H223AL2Receiver()
{

}

void H223AL2Receiver::Send(BYTE b)
{
	//Enque in sdu
	sdu.Push(b);
}

void H223AL2Receiver::SendClosingFlag()
{
	Debug("-AL2 Frame received\n");

	//Crc
	CRC8 crc;

	//Data
	BYTE *data;
	int dataLen;

	//Check minimum size
	if (sdu.Length()<1+useSN)
		goto clean;

	//Get data
	data = sdu.GetPointer();
	dataLen = sdu.Length();

	//Set data
	crc.Add(data,dataLen-useSN-1);

	//Calc
	if (data[dataLen]!=crc.Calc())
		goto clean;

	Debug("-AL2 Frame correct\n");

	//Enque new frame
	frameList.push_back(new H223MuxSDU(data+useSN,dataLen-useSN-1));

//Clean SDU and exit
clean:
	sdu.Clean();
}

H223MuxSDU* H223AL2Receiver::GetFrame()
{
	//Check size
	if (frameList.size()==0)
		return NULL;
	//Return first frame
	return frameList.front();
}

int H223AL2Receiver::NextFrame()
{
	//Check size
	if (frameList.size()==0)
		return 0;
	//Delete first
	delete frameList.front();
	//Remove
	frameList.pop_front();
	//Return size
	return frameList.size();
}


/****************** Sender **************/
H223AL2Sender::H223AL2Sender(int useSequenceNumbers)
{
	//Set sn parameter
	useSN = useSequenceNumbers;
	sn = 0;
}

H223AL2Sender::~H223AL2Sender()
{
}

H223MuxSDU* H223AL2Sender::GetNextPDU()
{
	//Check frame list
	if (frameList.size()==0)
		return NULL;

	//return first element
	return frameList.front();
}

void H223AL2Sender::OnPDUCompleted()
{
	//Get sdu
	H223MuxSDU *sdu = frameList.front();

	//Remove
	frameList.pop_front();

	//delete frame
	delete sdu;
}

int H223AL2Sender::SendPDU(BYTE *buffer,int len)
{
	//Crc
	CRC8 crc;

	//Build SDU
	H223MuxSDU *sdu = new H223MuxSDU();

	//If we have sn
	if (useSN)
		//Append
		sdu->Push(sn++);

	//Set buffer
	sdu->Push(buffer,len);

	//Calc crc
	crc.Add(sdu->GetPointer(),sdu->Length());

	//Append crc
	sdu->Push(crc.Calc());

	//Enque sdu
	frameList.push_back(sdu);

	//exit
	return true;
}

