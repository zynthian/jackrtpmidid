/* 
 * File:   JackRTPMIDID.cpp
 * RTP-MIDI daemon for Jack
 * Author: Benoit BOUCHEZ
 *
 * Created on 13 octobre 2019, 10:09
 
 *  Licensing terms
 *  This file and the rtpmidid project are licensed under GNU LGPL licensing terms
 *  Use of this source code in commercial applications and/or products without
 *  written agreement of the author is STRICTLY FORBIDDEN
 *  Refer to LICENSE.TXT file for details
 */

/*
 Command line options
 -verbosertp : displays applemidi session messages
 */

#include <stdio.h>
#include <stdlib.h>

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

#include <jack/jack.h>
#include <jack/midiport.h>
#include "RTP_MIDI.h"
#include "MIDI_FIFO.h"

jack_port_t *input_port;
jack_port_t *output_port;
//unsigned int FrameNumber=0;

CRTP_MIDI* RTPMIDIHandler=NULL;
TMIDI_FIFO_CHAR MIDI2JACK;
TMIDI_FIFO_CHAR JACK2RTP;

extern bool VerboseRTP;

// Function called when the RTP engine receives a valid MIDI message
// Stores received MIDI bytes into FIFO to JACK (generates a MIDI stream)
unsigned int RTPMIDICallback (void* Instance, unsigned int DataSize, unsigned char* DataBlock, unsigned int DeltaTime)
{
    unsigned int CurrentOutPtr;
    unsigned int TempInPtr;
    unsigned int ByteCount;
    bool Overflow=false;
        
    CurrentOutPtr=MIDI2JACK.ReadPtr;		// Make a snapshot to avoid change during next loop
    TempInPtr=MIDI2JACK.WritePtr;			// Local copy to be updated only when full MIDI message is transferred
	
    for (ByteCount=0; ByteCount<DataSize; ByteCount++)
    {
	MIDI2JACK.FIFO[TempInPtr]=DataBlock[ByteCount];
	TempInPtr+=1;
	if (TempInPtr>=MIDI_CHAR_FIFO_SIZE) TempInPtr=0;
	if (TempInPtr==CurrentOutPtr)
	{
            Overflow=true;
            break;
	}
    }
	
    if (Overflow==false)
    {  // Update input pointer only if we have been able to store all data
        MIDI2JACK.WritePtr=TempInPtr;
    }
    return 1;
}  // RTPMIDICallback
//-----------------------------------------------------------------------------

// Callback function called when there is an audio block to process
int jack_process(jack_nframes_t nframes, void *arg)
{
    int i;
    void* in_port_buf = jack_port_get_buffer(input_port, nframes);
    void* out_port_buf = jack_port_get_buffer(output_port, nframes);
    jack_midi_event_t in_event;
    jack_nframes_t event_count = jack_midi_get_event_count(in_port_buf);
    jack_midi_data_t* Buffer;
    unsigned int TempRead, LastBufferPos, TempWrite;
    unsigned char RunningStatus;
    unsigned int NumBytesToRead;
    unsigned char SYSEXBuffer[512];
    unsigned int SYSEXSize;
    unsigned char SYSEXByte;
    size_t NumBytesInEvent;
    unsigned int ByteCounter;
        
    jack_midi_clear_buffer(out_port_buf);    // Recommended to call this at the beginning of process cycle
    
    // Check if we have MIDI data waiting in the FIFO from RTP-MIDI to be sent
    if (MIDI2JACK.ReadPtr!=MIDI2JACK.WritePtr)
    {
        // Read FIFO and generate JACK events for each MIDI message in the FIFO
        TempRead=MIDI2JACK.ReadPtr;     // Local snapshot to avoid RTP-MIDI thread to see pointer moving while we parse the buffer
        LastBufferPos=MIDI2JACK.WritePtr;
        
        while (TempRead!=LastBufferPos)
        {
            // We can assume safely that there will be no incomplete MIDI message in the queue
            // as RTP-MIDI thread only transfers full MIDI messages. No need to check here
            // if a message in the queue is truncated
            
            // Identify message length from first byte
            RunningStatus=MIDI2JACK.FIFO[TempRead];
            
            if (RunningStatus==0xF0)
            {  // Specific SYSEX processing
                SYSEXBuffer[0]=0xF0;
                SYSEXSize=0;
                
                // Read SYSEX size by searching 0xF7
                do {
                    SYSEXByte=MIDI2JACK.FIFO[TempRead];
                    TempRead+=1;
                    if (TempRead>=MIDI_CHAR_FIFO_SIZE) TempRead=0;  // Could be a mask for faster update
                    
                    SYSEXBuffer[SYSEXSize]=SYSEXByte;
                    SYSEXSize++;
                } while ((SYSEXSize<512)&&(SYSEXByte!=0xF7));
                
                // If SYSEX is too big or 0xF7 not found, reject the message
                if (SYSEXByte==0xF7)
                {
                    // Allocate JACK buffer
                    Buffer=jack_midi_event_reserve (out_port_buf, 0, SYSEXSize);
                    if (Buffer!=0)
                    {  // Copy SYSEX message in the buffer
                        memcpy (Buffer, &SYSEXBuffer[0], SYSEXSize);
                    }
                }
            }
            else
            {  // Non SYSEX
                if ((RunningStatus>=0x80) && (RunningStatus<=0xBF)) NumBytesToRead=3;
                else if ((RunningStatus>=0xC0) && (RunningStatus<=0xDF)) NumBytesToRead=2;
                else if ((RunningStatus>=0xE0) && (RunningStatus<=0xEF)) NumBytesToRead=3;
                else if ((RunningStatus==0xF1)||(RunningStatus==0xF3)) NumBytesToRead=2;
                else if (RunningStatus==0xF2) NumBytesToRead=3;
                else NumBytesToRead=1;

                // Generate the message in JACK buffer
                Buffer=jack_midi_event_reserve (out_port_buf, 0, NumBytesToRead);
                if (Buffer!=0)
                {
                    Buffer[0]=RunningStatus;
                    TempRead+=1;
                    if (TempRead>=MIDI_CHAR_FIFO_SIZE) TempRead=0;  // Could be a mask for faster update

                    if (NumBytesToRead>1)       // Read first byte as we are alreay pointing it
                    {
                        Buffer[1]=MIDI2JACK.FIFO[TempRead];
                        TempRead+=1;
                        if (TempRead>=MIDI_CHAR_FIFO_SIZE) TempRead=0;  // Could be a mask for faster update
                    }

                    if (NumBytesToRead==3)      // Read second byte (avoid loop for standard MIDI messages)
                    {
                        Buffer[2]=MIDI2JACK.FIFO[TempRead];
                        TempRead+=1;
                        if (TempRead>=MIDI_CHAR_FIFO_SIZE) TempRead=0;  // Could be a mask for faster update
                    }
                }  // Buffer to JACK allocated
            }  // Non SYSEX message
        }  // loop over all events in the queue
        
        // Update read pointer only when we have parsed the whole buffer
        MIDI2JACK.ReadPtr=TempRead;
    }  // MIDI data available from RTP-MIDI queue
        
    // Generate RTP-MIDI payload (with null delta-time) for each event sent by JACK
    if(event_count >= 1)
    {
        //printf("rtpmidid: %d events\n", event_count);
        
        // Make a snapshot of current FIFO position
        TempWrite=JACK2RTP.WritePtr;

        for(i=0; i<event_count; i++)
        {
            jack_midi_event_get(&in_event, in_port_buf, i);
            NumBytesInEvent=in_event.size;
            
            // Try to store the event in the queue
            // Add leading null delta-time
            JACK2RTP.FIFO[TempWrite++]=0x00;
            if (TempWrite>=MIDI_CHAR_FIFO_SIZE) TempWrite=0;
            if (TempWrite==JACK2RTP.ReadPtr) return 0;      // FIFO is full discard the message
            // TODO : maybe we can save the TempWrite for the last valid message and send what has been stored successfully
            
            // Copy all the bytes from the JACK MIDI message to the queue
            for (ByteCounter=0; ByteCounter<NumBytesInEvent; ByteCounter++)
            {
                JACK2RTP.FIFO[TempWrite++]=in_event.buffer[ByteCounter];
                if (TempWrite>=MIDI_CHAR_FIFO_SIZE) TempWrite=0;
                if (TempWrite==JACK2RTP.ReadPtr) return 0;      // FIFO is full discard the message
            }
            
            /*
            if (*(in_event.buffer)!=0xFE)
            {
                printf("    event %d time is %d. 1st byte is 0x%x\n", i, in_event.time, *(in_event.buffer));
            }
            */
        }
        /*	printf("1st byte of 1st event addr is %p\n", in_events[0].buffer);*/
        
        // Update FIFO pointer when all events to send have been read
        JACK2RTP.WritePtr=TempWrite;
    }
       
    return 0;      
}  // jack_process
// ----------------------------------------------------

/* Callback function called when jack server is shut down */
void jack_shutdown(void *arg)
{
	exit(1);
}  // jack_shutdown
// ----------------------------------------------------

int main(int argc, char** argv)
{
    int Ret;
    jack_client_t *client;
    
    printf ("JACK <-> RTP-MIDI bridge V0.4 for Zynthian\n");
    printf ("Copyright 2019/2020 Benoit BOUCHEZ (BEB)\n");
    printf ("Please report any issue to BEB on https:\\discourse.zynthian.org\n");
    
    MIDI2JACK.ReadPtr=0;
    MIDI2JACK.WritePtr=0;
    
    JACK2RTP.ReadPtr=0;
    JACK2RTP.WritePtr=0;
    
    if (argc>=2)
    {
        if (strcmp(argv[1], "-verbosertp")==0) VerboseRTP=true;
    }

    if ((client = jack_client_open ("jackrtpmidid", JackNullOption, NULL)) == 0)
    {
        fprintf(stderr, "jackrtpmidid : JACK server not running\n");
        return 1;
    }
        
    RTPMIDIHandler = new CRTP_MIDI (&JACK2RTP, 0, 0, 0, &RTPMIDICallback, 0);
    if (RTPMIDIHandler)
    {
        RTPMIDIHandler->setSessionName((char*)"Zynthian RTP-MIDI");
        Ret=RTPMIDIHandler->InitiateSession (0, 5004, 5005, 5004, 5005, false);
        if (Ret==-1) fprintf (stderr, "jackrtpmidid : can not create control socket\n");
        else if (Ret==-2) fprintf (stderr, "jackrtpmidid : can not create data socket\n");
        if (Ret!=0) 
        {
            delete RTPMIDIHandler;
            return 1;
        }
    }

    // Register the various callbacks needed by a JACK application
    jack_set_process_callback (client, jack_process, 0);
    jack_on_shutdown (client, jack_shutdown, 0);

    input_port = jack_port_register (client, "rtpmidi_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    output_port = jack_port_register (client, "rtpmidi_out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);

    if (jack_activate (client))
    {
            fprintf(stderr, "jackrtpmidid : cannot activate client");
            return 1;
    }

    /* run until interrupted */
    while(1)
    {
        if (RTPMIDIHandler) RTPMIDIHandler->RunSession();
        SystemWaitMS(1);        // Run RTP-MIDI process every millisecond
    }
    jack_client_close(client);
     
    if (RTPMIDIHandler)
    {
        RTPMIDIHandler->CloseSession();
        delete RTPMIDIHandler;
        RTPMIDIHandler=0;
    }
        
    exit (0);
        
    return (EXIT_SUCCESS);
}  // main
// ----------------------------------------------------
