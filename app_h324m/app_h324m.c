/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Sergio Garcia Murillo <sergio.garcia@ydilo.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief H324M stack
 * 
 * \ingroup applications
 */

#include <asterisk.h>

#include <h324m.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <asterisk/lock.h>
#include <asterisk/file.h>
#include <asterisk/logger.h>
#include <asterisk/channel.h>
#include <asterisk/pbx.h>
#include <asterisk/module.h>
#include <asterisk/causes.h>

#ifndef AST_FORMAT_AMR
#define AST_FORMAT_AMR (1 << 13)
#endif

static char *name_h324m_loopback = "h324m_loopback";
static char *syn_h324m_loopback = "H324m loopback mode";
static char *des_h324m_loopback = "  h324m_loopback():  Estabish connection and loopback media.\n";

static char *name_h324m_gw = "h324m_gw";
static char *syn_h324m_gw = "H324m gateway";
static char *des_h324m_gw = "  h324m_gw():  Creates a pseudo channel for an incoming h324m call.\n";

static char *name_h324m_call = "h324m_call";
static char *syn_h324m_call = "H324m call";
static char *des_h324m_call = "  h324m_call():  Creates a pseudo channel for an outgoing h324m call.\n";

static char *name_video_loopback = "video_loopback";
static char *syn_video_loopback = "video_loopback";
static char *des_video_loopback = "  video_loopback():  Video loopback.\n";

static short blockSize[16] = { 12, 13, 15, 17, 19, 20, 26, 31,  5, -1, -1, -1, -1, -1, -1, -1};

struct video_tr
{
	unsigned char tr;
	unsigned int samples;
};

static struct ast_frame* create_ast_frame(void *frame, struct video_tr *vtr)
{
	int mark = 0;
	struct ast_frame* send;

	/* Get data & size */
	unsigned char * framedata = FrameGetData(frame);
	unsigned int framelength = FrameGetLength(frame);

	/* Depending on the type */
	switch(FrameGetType(frame))
	{
		case MEDIA_AUDIO:
			/*Check it's AMR */
			if (FrameGetCodec(frame)!=CODEC_AMR)
				/* exit */
				return NULL;
			/* Create frame */
			send = (struct ast_frame *) malloc(sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET + framelength + 2);
			/* Set data*/
			send->data = (void*)send + AST_FRIENDLY_OFFSET;
			send->datalen = framelength;
			/* Set header cmr */
			((unsigned char*)(send->data))[0] = 0xF0;
			/* Set mode */
			((unsigned char*)(send->data))[1] = (framedata[0] & 0x78) | 0x04;
			/* Copy */
			memcpy(send->data+2, framedata, framelength);
			/* Set video type */
			send->frametype = AST_FRAME_VOICE;
			/* Set codec value */
			send->subclass = AST_FORMAT_AMR;
			/* Rest of values*/
			send->src = "h324m";
			send->samples = 160;
			send->delivery.tv_usec = 0;
			send->delivery.tv_sec = 0;
			send->mallocd = 0;
			/* Send */
			return send;
		case MEDIA_VIDEO:
			/*Check it's H263 */
			if (FrameGetCodec(frame)!=CODEC_H263)
				/* exit */
				return NULL;
			/* Create frame */
			send = (struct ast_frame *) malloc(sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET + 2 + framelength);
			/* if it�s first */
			if (framedata[0]==0 && framedata[1]==0)
			{
				/* Get time reference */
				unsigned char tr = (framedata[0] << 6) & 0xC0; 	// 2 LS bits out of the 3rd byte
				tr |= (framedata[1] >> 2) & 0x3F; 	// 6 MS bits out of the 4th byte
				/* calculate samples */
				if (tr < vtr->tr)
					vtr->samples = ((256+tr) - vtr->tr) * 1000;
				else
					vtr->samples = (tr - vtr->tr) * 1000;
				/* Save tr */
				vtr->tr = tr;
				/* No data*/
				send->data = (void*)send + AST_FRIENDLY_OFFSET;
				send->datalen = framelength;
				/* Copy */
				memcpy(send->data+2, framedata+2, framelength-2);
				/* Set header */
				((unsigned char*)(send->data))[0] = 0x04;
				((unsigned char*)(send->data))[1] = 0x00; 
				/* Set mark */
				mark = 1;
			} else {
				/* No data*/
				send->data = (void*)send + AST_FRIENDLY_OFFSET;
				send->datalen =  framelength + 2  ;
				/* Copy */
				memcpy(send->data+2, framedata, framelength);
				/* Set header */
				((unsigned char*)(send->data))[0] = 0x00;
				((unsigned char*)(send->data))[1] = 0x00;
				/* Unset mark */
				mark = 0;
			}
			/* Set video type */
			send->frametype = AST_FRAME_VIDEO;
			/* Set codec value */
			send->subclass = AST_FORMAT_H263_PLUS | mark;
			/* Rest of values*/
			send->src = "h324m";
			send->samples = vtr->samples;
			send->delivery.tv_usec = 0;
			send->delivery.tv_sec = 0;
			send->mallocd = 0;
			/* Send */
			return send;
	}

	/* NOthing */
	return NULL;
}

struct h324m_packetizer
{
	unsigned char *framedata;
	unsigned char *offset;
        int framelength;
	int num;
	int max;
};

static int init_h324m_packetizer(struct h324m_packetizer *pak,struct ast_frame* f)
{
	int i;

	/* Empty data */
	memset(pak,0,sizeof(struct h324m_packetizer));

	/* Depending on the type */
	switch (f->frametype)
	{
		case AST_FRAME_VOICE:
			/* Check audio type */
			if (!(f->subclass & AST_FORMAT_AMR))
				/* exit */
				return 0;
			/* Get data & length */
			pak->framedata = (unsigned char *)f->data;
			pak->framelength = f->datalen;
			/* Read toc until no mark found, skip first byte */
			while ((++pak->max<pak->framelength) && (pak->framedata[pak->max] & 0x80)) {}
			/* Check lenght */
			if (pak->max >= pak->framelength)
				/* Exit */	
				return 0;
			/* Set offset */
			pak->offset = pak->framedata + pak->max + 1;
			/* Move toc to the beggining so we can overwrite the byte before the frame */
			for (i=0;i<pak->max;i++)
				/* copy */
				pak->framedata[i] = pak->framedata[i+1];
			/* Good one */
			return 1;
		case AST_FRAME_VIDEO:
			/* Depending on the codec */
			if (f->subclass & AST_FORMAT_H263) 
			{
				/* Get data & length without rfc 2190 (only A packets ) */
				pak->framedata = (unsigned char *)f->data+4;
				pak->framelength = f->datalen-4;
			} else if (f->subclass & AST_FORMAT_H263_PLUS) {
				/* Get initial data */
				pak->framedata = (unsigned char *)f->data;
				pak->framelength = f->datalen;
				/* Get header */
				unsigned char p = pak->framedata[0] & 0x04;
				unsigned char v = pak->framedata[0] & 0x02;
				unsigned char plen = ((pak->framedata[0] & 0x1 ) << 5 ) | (pak->framedata[1] >> 3);
				unsigned char pebit = pak->framedata[0] & 0x7;
				/* skip header*/
				pak->framedata += 2+plen;
				pak->framelength -= 2+plen;
				/* Check */
				if (v)
				{
					/* Increase ini */
					pak->framedata++;
					pak->framelength--;
				}
				/* Check p bit */
				if (p)
				{
					/* Decrease ini */
					pak->framedata -= 2;
					pak->framelength += 2;
					/* Append 0s */	
					pak->framedata[0] = 0;
					pak->framedata[1] = 0;
				}
			} else
				break;
			/* Only 1 packet */
			pak->max = 1;
			/* Exit */
			return 1;

	}
	/* Nothing to do */
	return 0;
}

static void* create_h324m_frame(struct h324m_packetizer *pak,struct ast_frame* f)
{
	/* if not more */
	if (pak->num++ == pak->max)
		/* Exit */
		return NULL;

	/* Depending on the type */
	switch (f->frametype)
	{
		case AST_FRAME_VOICE:
		{
			/* Check audio type */
			if (!(f->subclass & AST_FORMAT_AMR))
				/* exit */
				break;
			/* Get mode */
			unsigned char mode = pak->framedata[pak->num-1] >> 3 & 0x0f;
			/* Get blockSize */
			unsigned bs = blockSize[mode];
			/* Overwrite previous byte with header */
			pak->offset[-1] = (mode << 3) | 0x04;;
			/* Inc offset first */
			pak->offset += bs;
			/* Create frame */	
			return FrameCreate(MEDIA_AUDIO, CODEC_AMR, pak->offset - bs - 1, bs + 1);
		}
		case AST_FRAME_VIDEO:
			/* Create frame */
			return FrameCreate(MEDIA_VIDEO, CODEC_H263, pak->framedata, pak->framelength);
	}
	/* NOthing */
	return NULL;
}

static int app_h324m_loopback(struct ast_channel *chan, void *data)
{
	struct ast_frame *f;
	struct ast_module_user *u;
	void*  frame;

	ast_log(LOG_DEBUG, "h324m_loopback\n");

	/* Lock module */
	u = ast_module_user_add(chan);

	/* Create session */
	void* id = H324MSessionCreate();

	/* Init session */
	H324MSessionInit(id);

	/* Wait for data avaiable on channel */
	while (ast_waitfor(chan, -1) > -1) {

		/* Read frame from channel */
		f = ast_read(chan);

		/* if it's null */
		if (f == NULL)
			break;

		/* Check frame type */
		if (f->frametype == AST_FRAME_VOICE) {
			/* read data */
			H324MSessionRead(id, (unsigned char *)f->data, f->datalen);
			/* Get frames */
			while ((frame=H324MSessionGetFrame(id))!=NULL)
			{
				/* If it's video */
				if (FrameGetType(frame)==MEDIA_VIDEO)
					/* Send it back */
					H324MSessionSendFrame(id,frame);
				/* Delete frame */
				FrameDestroy(frame);
			}
			/* write data */
			H324MSessionWrite(id, (unsigned char *)f->data, f->datalen);
			/* deliver now */
			f->delivery.tv_usec = 0;
			f->delivery.tv_sec = 0;
			/* write frame */
			ast_write(chan, f);
		} 
	}

	/* Destroy session */
	H324MSessionEnd(id);

	/* Destroy session */
	H324MSessionDestroy(id);

	ast_log(LOG_DEBUG, "exit");

	/* Unlock module*/
	ast_module_user_remove(u);

	//Exit
	return 0;
}

static int app_h324m_gw(struct ast_channel *chan, void *data)
{
	struct ast_frame *f;
	struct ast_frame *send;
	struct ast_module_user *u;
	struct h324m_packetizer pak;
	struct video_tr vtr= {0,0};
	void*  frame;
	char*  input;
	int    reason = 0;
	int    ms;
	struct ast_channel *channels[2];
	struct ast_channel *pseudo;
	struct ast_channel *where;

	ast_log(LOG_DEBUG, "h324m_loopback\n");

	/* Lock module */
	u = ast_module_user_add(chan);

	/* Request new channel */
	pseudo = ast_request("Local", AST_FORMAT_H263 | AST_FORMAT_H263_PLUS | AST_FORMAT_AMR, data, &reason);
 
	/* If somthing has gone wrong */
	if (!pseudo)
		/* goto end */
		goto end; 

	/* Set caller id */
	ast_set_callerid(pseudo, chan->cid.cid_num, chan->cid.cid_name, chan->cid.cid_num);

	/* Place call */
	if (ast_call(pseudo,data,0))
		/* if fail goto clean */
		goto clean_pseudo;

	/* while not setup */
	while (pseudo->_state!=AST_STATE_UP) {
		/* Wait for data */
		if (ast_waitfor(pseudo, 0)<0)
			/* error, timeout, or done */
			break;
		/* Read frame */
		f = ast_read(pseudo);
		/* If not frame */
		if (!f)
			/* done */ 
			break;
		/* If it's a control frame */
		if (f->frametype == AST_FRAME_CONTROL) {
			/* Dependinf on the event */
			switch (f->subclass) {
				case AST_CONTROL_RINGING:       
					break;
				case AST_CONTROL_BUSY:
				case AST_CONTROL_CONGESTION:
					/* Save cause */
					reason = pseudo->hangupcause;
					/* exit */
					goto hangup_pseudo;
					break;
				case AST_CONTROL_ANSWER:
					/* Set UP*/
					reason = 0;	
					break;
			}
		}
		/* Delete frame */
		ast_frfree(f);
	}

	/* If no answer */
	if (pseudo->_state != AST_STATE_UP)
		/* goto end */
		goto clean_pseudo; 

	/* Create session */
	void* id = H324MSessionCreate();

	/* Init session */
	H324MSessionInit(id);

	/* Answer call */
	ast_answer(chan);

	/* Set up array */
	channels[0] = chan;
	channels[1] = pseudo;

	/* No timeout */
	ms = -1;

	/* Wait for data avaiable on any channel */
	while (!reason && (where = ast_waitfor_n(channels, 2, &ms)) != NULL) {
		/* Read frame from channel */
		f = ast_read(where);

		/* if it's null */
		if (f == NULL)
			break;

		/* If it's on h324m channel */
		if (where==chan) {
			/* Check frame type */
			if (f->frametype == AST_FRAME_VOICE) {
				/* read data */
				H324MSessionRead(id, (unsigned char *)f->data, f->datalen);
				/* Get frames */
				while ((frame=H324MSessionGetFrame(id))!=NULL)
				{
					/* Packetize outgoing frame */
					if ((send=create_ast_frame(frame,&vtr))!=NULL)
						/* Send frame */
						ast_write(pseudo,send);
					/* Delete frame */
					FrameDestroy(frame);
				}
				/* Get user input */
				while((input=H324MSessionGetUserInput(id))!=NULL)
				{
					/* Send digit begin */
					ast_senddigit_begin(pseudo,input[0]);
					/* Send digit end */
					ast_senddigit_end(pseudo,input[0]);
					/* free data */
					free(input);
				}

				/* write data */
				H324MSessionWrite(id, (unsigned char *)f->data, f->datalen);
				/* deliver now */
				f->delivery.tv_usec = 0;
				f->delivery.tv_sec = 0;
				/* write frame */
				ast_write(chan, f);

			} else if (f->frametype == AST_FRAME_CONTROL) {
				/* Check for hangup */
				if (f->subclass == AST_CONTROL_HANGUP)
					/* exit */
					reason = AST_CAUSE_NORMAL_CLEARING;
			}

			/* Delete frame */
			ast_frfree(f);
		} else {
			/* Check for hangup */
			if ((f->frametype == AST_FRAME_CONTROL)&& (f->subclass == AST_CONTROL_HANGUP)) {
				/* exit */
				reason = AST_CAUSE_NORMAL_CLEARING;
			/* Check for DTMF */
			} else if (f->frametype == AST_FRAME_DTMF) {
				
			} else {
				/* Init packetizer */
				if (init_h324m_packetizer(&pak,f))
					/* Create frame */
					while ((frame=create_h324m_frame(&pak,f))!=NULL) {
						/* Send frame */
						H324MSessionSendFrame(id,frame);
						/* Delete frame */
						FrameDestroy(frame);
					}
			}
			/* Delete frame */
			ast_frfree(f);
		}
	}

	/* End session */
	H324MSessionEnd(id);

	/* Destroy session */
	H324MSessionDestroy(id);

hangup_pseudo:
	/* Hangup pseudo channel if needed */
	ast_softhangup(pseudo, reason);

clean_pseudo:
	/* Destroy pseudo channel */
	ast_hangup(pseudo);

end:
	/* Hangup channel if needed */
	ast_softhangup(chan, reason);

	/* Unlock module*/
	ast_module_user_remove(u);

	//Exit
	return -1;
}

static int app_h324m_call(struct ast_channel *chan, void *data)
{
	struct ast_frame *f;
	struct ast_frame *send;
	struct ast_module_user *u;
	struct h324m_packetizer pak;
	struct video_tr vtr = {0,0};
	void*  frame;
	char*  input;
	int    reason = 0;
	int    ms;
	struct ast_channel *channels[2];
	struct ast_channel *pseudo;
	struct ast_channel *where;

	ast_log(LOG_DEBUG, "h324m_call\n");

	/* Lock module */
	u = ast_module_user_add(chan);

	/* Request new channel */
	pseudo = ast_request("Local", AST_FORMAT_ALAW | AST_FORMAT_ULAW, data, &reason);
 
	/* If somthing has gone wrong */
	if (!pseudo)
		/* goto end */
		goto end; 

	/* Set caller id */
	ast_set_callerid(pseudo, chan->cid.cid_num, chan->cid.cid_name, chan->cid.cid_num);

	/* Place call */
	if (ast_call(pseudo,data,0))
		/* if fail goto clean */
		goto clean_pseudo;

	/* while not setup */
	while (pseudo->_state!=AST_STATE_UP) {
		/* Wait for data */
		if (ast_waitfor(pseudo, 0)<0)
			/* error, timeout, or done */
			break;
		/* Read frame */
		f = ast_read(pseudo);
		/* If not frame */
		if (!f)
			/* done */ 
			break;
		/* If it's a control frame */
		if (f->frametype == AST_FRAME_CONTROL) {
			/* Dependinf on the event */
			switch (f->subclass) {
				case AST_CONTROL_RINGING:       
					break;
				case AST_CONTROL_BUSY:
				case AST_CONTROL_CONGESTION:
					/* Save cause */
					reason = pseudo->hangupcause;
					/* exit */
					goto hangup_pseudo;
					break;
				case AST_CONTROL_ANSWER:
					/* Set UP*/
					reason = 0;	
					break;
			}
		}
		/* Delete frame */
		ast_frfree(f);
	}

	/* If no answer */
	if (pseudo->_state != AST_STATE_UP)
		/* goto end */
		goto clean_pseudo; 

	/* Create session */
	void* id = H324MSessionCreate();

	/* Init session */
	H324MSessionInit(id);

	/* Answer call */
	ast_answer(chan);

	/* Set up array */
	channels[0] = chan;
	channels[1] = pseudo;

	/* No timeout */
	ms = -1;

	/* Create enpty packet */
	send = (struct ast_frame *) malloc(sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET + 160 );
	/* No data*/
	send->data = (void*)send + AST_FRIENDLY_OFFSET;
	send->datalen = 160;
	/* Set DTMF type */
	send->frametype = AST_FRAME_VOICE;
	/* Set DTMF value */
	send->subclass = pseudo->rawwriteformat;
	/* Rest of values*/
	send->src = 0;
	send->samples = 160;
	send->delivery.tv_usec = 0;
	send->delivery.tv_sec = 0;
	send->mallocd = 0;
	/* Send */
	ast_write(pseudo,send);

	/* Wait for data avaiable on any channel */
	while (!reason && (where = ast_waitfor_n(channels, 2, &ms)) != NULL) {
		/* Read frame from channel */
		f = ast_read(where);

		/* if it's null */
		if (f == NULL)
			break;

		/* If it's on h324m channel */
		if (where==pseudo) {
			/* Check frame type */
			if (f->frametype == AST_FRAME_VOICE) {
				/* read data */
				H324MSessionRead(id, (unsigned char *)f->data, f->datalen);
				/* Get frames */
				while ((frame=H324MSessionGetFrame(id))!=NULL)
				{
					/* Packetize outgoing frame */
					if ((send=create_ast_frame(frame,&vtr))!=NULL)
						/* Send frame */
						ast_write(chan,send);
					/* Delete frame */
					FrameDestroy(frame);
				}
				/* Get user input */
				while((input=H324MSessionGetUserInput(id))!=NULL)
				{
					/* Send digit begin */
					ast_senddigit_begin(pseudo,input[0]);
					/* Send digit end */
					ast_senddigit_end(pseudo,input[0]);
					/* free data */
					free(input);
				}

				/* write data */
				H324MSessionWrite(id, (unsigned char *)f->data, f->datalen);
				/* deliver now */
				f->delivery.tv_usec = 0;
				f->delivery.tv_sec = 0;
				/* write frame */
				ast_write(pseudo, f);
			} else if (f->frametype == AST_FRAME_CONTROL) {
				/* Check for hangup */
				if (f->subclass == AST_CONTROL_HANGUP) 
					/* exit */
					reason = AST_CAUSE_NORMAL_CLEARING;
			} else 
				/* Delete frame */
				ast_frfree(f);
		} else {
			/* Check frame type DTMF*/
			if (f->frametype == AST_FRAME_DTMF) {
				char dtmf[2];
				/* Get DTMF */
				dtmf[0] = f->subclass;
				dtmf[1] = 0;
				/* Send DTMF */
				H324MSessionSendUserInput(id,dtmf);	
			/* Check control channel */
			} else if (f->frametype == AST_FRAME_CONTROL) {
				/* Check for hangup */
				if (f->subclass == AST_CONTROL_HANGUP)
					/* exit */
					reason = AST_CAUSE_NORMAL_CLEARING;
				/* Init packetizer */
			} else if (init_h324m_packetizer(&pak,f)) {
				/* Create frame */
				while ((frame=create_h324m_frame(&pak,f))!=NULL) {
					/* Send frame */
					H324MSessionSendFrame(id,frame);
					/* Delete frame */
					FrameDestroy(frame);
				}
			}
			/* Delete frame */
			ast_frfree(f);
		}
	}

	/* End session */
	H324MSessionEnd(id);

	/* Destroy session */
	H324MSessionDestroy(id);

hangup_pseudo:
	/* Hangup pseudo channel if needed */
	ast_softhangup(pseudo, reason);

clean_pseudo:
	/* Destroy pseudo channel */
	ast_hangup(pseudo);

end:
	/* Hangup channel if needed */
	ast_softhangup(chan, reason);

	/* Unlock module*/
	ast_module_user_remove(u);

	//Exit
	return -1;
}

static int app_video_loopback(struct ast_channel *chan, void *data)
{
	struct ast_frame *f;
	struct ast_module_user *u;

	ast_log(LOG_DEBUG, "video_loopback\n");

	/* Lock module */
	u = ast_module_user_add(chan);

	/* Wait for data avaiable on channel */
	while (ast_waitfor(chan, -1) > -1) {

		/* Read frame from channel */
		f = ast_read(chan);

		/* if it's null */
		if (f == NULL)
			break;

		/* Check frame type */
		if (f->frametype == AST_FRAME_VIDEO) {
			/* deliver now */
			f->delivery.tv_usec = 0;
			f->delivery.tv_sec = 0;
			/* write frame */
			ast_write(chan, f);
		} 
	}

	ast_log(LOG_DEBUG, "exit");

	/* Unlock module*/
	ast_module_user_remove(u);

	//Exit
	return 0;
}

static int unload_module(void)
{
	int res;

	res = ast_unregister_application(name_h324m_loopback);
	res &= ast_unregister_application(name_h324m_gw);
	res &= ast_unregister_application(name_h324m_call);
	res &= ast_unregister_application(name_video_loopback);

	ast_module_user_hangup_all();

	return res;
}

static int load_module(void)
{
	int res;

	res = ast_register_application(name_h324m_loopback, app_h324m_loopback, syn_h324m_loopback, des_h324m_loopback);
	res &= ast_register_application(name_h324m_gw, app_h324m_gw, syn_h324m_gw, des_h324m_gw);
	res &= ast_register_application(name_h324m_call, app_h324m_call, syn_h324m_call, des_h324m_call);
	res &= ast_register_application(name_video_loopback, app_video_loopback, syn_video_loopback, des_video_loopback);
	return 0;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "H324M stack");

