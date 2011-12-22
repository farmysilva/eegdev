/*
    Copyright (C) 2011  EPFL (Ecole Polytechnique Fédérale de Lausanne)
    Nicolas Bourdaud <nicolas.bourdaud@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <unistd.h>
#include <pthread.h>
#include <expat.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <eegdev-common.h>
#include "device-helper.h"
#include "cross-socket.h"

/*****************************************************************
 *                        TOBIIA metadata                        *
 *****************************************************************/
struct signal_information {
	const char* type;
	uint32_t mask;
	int aperiodic;
	int isint;
	const char *unit, *trans, *filt;
};

static const struct signal_information sig_info[] = {
	{.type = "eeg", .mask = 0x00000001, .unit = "uV"},
	{.type = "emg", .mask = 0x00000002, .unit = "uV"},
	{.type = "eog", .mask = 0x00000004, .unit = "uV"},
	{.type = "ecg", .mask = 0x00000008},
	{.type = "hr", .mask = 0x00000010},
	{.type = "bp", .mask = 0x00000020},
	{.type = "button", .mask = 0x00000040, .unit = "Boolean",
	               .aperiodic = 1, .isint = 1, .filt = "No filtering"},
	{.type = "joystick", .mask = 0x00000080,
	                           .aperiodic = 1, .filt = "No filtering"},
	{.type = "sensors", .mask = 0x00000100},
	{.type = "nirs", .mask = 0x00000200},
	{.type = "fmri", .mask = 0x00000400},
	{.type = "mouse", .mask = 0x00000800,
	                           .aperiodic = 1, .filt = "No filtering"},
	{.type = "mouse-button", .mask = 0x00001000, .unit = "Boolean",
	                           .aperiodic = 1, .filt = "No filtering"},
	{.type = "user1", .mask = 0x00010000},
	{.type = "user2", .mask = 0x00020000},
	{.type = "user3", .mask = 0x00040000},
	{.type = "user4", .mask = 0x00080000},
	{.type = "undefined", .mask = 0x00100000},
	{.type = "event", .mask = 0x00200000, .unit = "Boolean", 
	            .trans = "Triggers and Status", .filt = "No filtering"}
};
#define TIA_NUM_SIG (sizeof(sig_info)/sizeof(sig_info[0]))

static const char unknown_field[] = "Unknown";

static const char tia_device_type[] = "TOBI interface A";
#define DEFAULTHOST	"localhost"
#define DEFAULTPORT	"38500"


#define XML_BSIZE	4096

struct parsingdata {
	int sig, nch;
	char ltype[16];
};

struct tia_eegdev {
	struct eegdev dev;
	FILE* ctrl;
	int datafd, ctrlfd;
	pthread_t thid;
	XML_Parser parser;

	int fs, blocksize, invalid;
	unsigned int nch, nsig;
	int offset[TIA_NUM_SIG];

	struct egdich* chmap;

	struct parsingdata pdata;
};
#define get_tia(dev_p) 	((struct tia_eegdev*)(dev_p))

/*****************************************************************
 *                        tobiia misc                            *
 *****************************************************************/
static
int get_eegdev_sigtype(const char* sig)
{
	int sigid;

	if (!strcmp(sig, "eeg"))
		sigid = EGD_EEG;	
	else if (!strcmp(sig, "event"))
		sigid = EGD_TRIGGER;
	else
		sigid = EGD_SENSOR;
	
	return sigid;
}

static
int get_tobiia_siginfo_mask(uint32_t type)
{
	int i;

	for (i=0; i<(int)TIA_NUM_SIG; i++)
		if (type == sig_info[i].mask)
			return i;
	
	return -1;
}

static
int get_tobiia_siginfo_type(const char* ltype)
{
	int i;

	for (i=0; i<(int)TIA_NUM_SIG; i++)
		if (!strcmp(ltype,sig_info[i].type))
			return i;
	
	return -1;
}


static
int parse_url(const char* url, char* host, unsigned short *port)
{
	if (!sscanf(url, "%[^][:]:%hu", host, port)
	 && !sscanf(url, "%[:0-9a-f]", host)
	 && !sscanf(url, "[%[:0-9a-f]]:%hu", host, port)) {
		fprintf(stderr, "Cannot parse address\n");
		return -1;
	}

	return 0;
}


static
int connect_server(const char *host, unsigned int short port)
{
	struct addrinfo *res, hints = {.ai_socktype = SOCK_STREAM};
	int fd, error;
	char portnum[8];
	
	// Name resolution
	sprintf(portnum, "%u", port);
	if ((error = getaddrinfo(host, portnum, &hints, &res))) {
		fprintf(stderr, "failed: %s\n", gai_strerror(error));
		return -1;
	}

	// Create and connect socket
	if ((fd = sock_socket(AF_INET, SOCK_STREAM, 0)) < 0
	  || sock_connect(fd, res->ai_addr, res->ai_addrlen)) {
		if (fd >= 0)
			close(fd);
		fd = -1;
	}
		

	freeaddrinfo(res);
	return fd;
}


/*****************************************************************
 *                        tobiia XML parsing                     *
 *****************************************************************/
static
int ch_cmp(const void* e1, const void* e2)
{
	const struct egdich *ch1 = e1, *ch2 = e2;
	const struct signal_information *tsinfo1, *tsinfo2;

	tsinfo1 = ch1->data;
	tsinfo2 = ch2->data;

	if (tsinfo1->mask == tsinfo2->mask)
		return 0;
	else if (tsinfo1->mask < tsinfo2->mask)
		return -1;
	else
		return 1;
}


static
void parse_start_tiametainfo(struct tia_eegdev* tdev)
{
	unsigned int i;

	for (i=0; i<TIA_NUM_SIG; i++)
		tdev->offset[i] = -1;
}


static
void parse_end_tiametainfo(struct tia_eegdev* tdev)
{
	unsigned int i;
	int signch, nch = 0;

	qsort(tdev->chmap, tdev->nch, sizeof(*tdev->chmap), ch_cmp);

	for (i=0; i<TIA_NUM_SIG; i++) {
		if (tdev->offset[i] < 0)
			continue;
		signch = tdev->offset[i]+1;
		tdev->offset[i] = nch;
		nch += signch;
	}
}


static
int parse_start_mastersignal(struct tia_eegdev* tdev, const char **attr)
{
	unsigned int i;
	
	// Read signal metadata
 	for (i=0; attr[i]; i+=2) {
		if (!strcmp(attr[i], "samplingRate"))
			tdev->dev.cap.sampling_freq = atoi(attr[i+1]);
		else if (!strcmp(attr[i], "blockSize"))
			tdev->blocksize = atoi(attr[i+1]);
	}

	return 0;
}


static
int parse_start_signal(struct tia_eegdev* tdev, const char **attr)
{
	unsigned int i, fs = 0;
	int sig, tiatype, bs = 0;
	struct egdich *newchmap = tdev->chmap;
	const char* ltype = NULL;
	
	// Read signal metadata
 	for (i=0; attr[i]; i+=2) {
		if (!strcmp(attr[i], "type"))
			ltype = attr[i+1];
		else if (!strcmp(attr[i], "numChannels"))
			tdev->pdata.nch = atoi(attr[i+1]);
		else if (!strcmp(attr[i], "samplingRate"))
			fs = atoi(attr[i+1]);
		else if (!strcmp(attr[i], "blockSize"))
			bs = atoi(attr[i+1]);
	}

	// For the moment we support only signal with the same samplerate
	// as the mastersignal
	if ((tdev->dev.cap.sampling_freq != fs) || (tdev->blocksize != bs)) 
		return -1;

	tdev->nsig++;
	sig = get_eegdev_sigtype(ltype);
	tdev->dev.cap.type_nch[sig] += tdev->pdata.nch;

	// resize metadata structures to hold new channels
	tdev->nch += tdev->pdata.nch;
	newchmap = realloc(newchmap, tdev->nch*sizeof(*newchmap));
	if (!newchmap)
		return -1;
	tdev->chmap = newchmap;

	tiatype = get_tobiia_siginfo_type(ltype);
	if (tiatype < 0)
		return -1;
	tdev->offset[tiatype] += tdev->pdata.nch;
	
	for (i=tdev->nch - tdev->pdata.nch; i<tdev->nch; i++) {
		tdev->chmap[i].stype = sig;
		tdev->chmap[i].dtype = EGD_FLOAT;
		tdev->chmap[i].label = NULL;
		tdev->chmap[i].data = &sig_info[tiatype];
	}
	tdev->pdata.sig = sig;
	strncpy(tdev->pdata.ltype, ltype, sizeof(tdev->pdata.ltype)-1);

	return 0;
}


static
int parse_end_signal(struct tia_eegdev* tdev)
{
	int i;
	size_t len = strlen(tdev->pdata.ltype)+8;
	struct egdich* newmap = tdev->chmap + (tdev->nch - tdev->pdata.nch);
	char* label;
	
	// Assign default labels for unlabelled channels
	for (i=0; i<tdev->pdata.nch; i++) {
		if (!newmap[i].label) {
			if (!(label = malloc(len)))
				return -1;
			sprintf(label, "%s:%u", tdev->pdata.ltype, i+1);
			newmap[i].label = label;
		}
	}

	return 0;
}


static
int parse_start_channel(struct tia_eegdev* tdev, const char **attr)
{
	int index = -1, i, oldnch, sig;
	const char* label = NULL;
	char* newlabel;

 	for (i=0; attr[i]; i+=2) {
		if (!strcmp(attr[i], "nr"))
			index = atoi(attr[i+1])-1;
		else if (!strcmp(attr[i], "label"))
			label = attr[i+1];
	}

	// locate the channel to modify
	if (index >= tdev->pdata.nch || index < 0)
		return -1;
	sig = tdev->pdata.sig;
	oldnch = tdev->nch - tdev->pdata.nch;
	i = egdi_next_chindex(tdev->chmap + oldnch, sig, index) + oldnch;
	
	// Change the label
	if (!(newlabel = realloc(tdev->chmap[i].label, strlen(label)+1)))
		return -1;
	strcpy(newlabel, label);
	tdev->chmap[i].label = newlabel;
	
	return 0;
}


static XMLCALL
void start_xmlelt(void *data, const char *name, const char **attr)
{
	struct tia_eegdev* tdev = data;
	int ret = 0;

	if (!strcmp(name, "tiaMetaInfo")) 
		parse_start_tiametainfo(tdev);
	else if (!strcmp(name, "masterSignal"))
	      ret = parse_start_mastersignal(tdev, attr);
	else if (!strcmp(name, "signal"))
	      ret = parse_start_signal(tdev, attr);
	else if (!strcmp(name, "channel"))
	      ret = parse_start_channel(tdev, attr);

	if (ret) {
		tdev->invalid = 1;
		XML_StopParser(tdev->parser, XML_FALSE);
	}	
}


static XMLCALL
void end_xmlelt(void *data, const char *name)
{
	struct tia_eegdev* tdev = data;

	if (!strcmp(name, "signal")) {
		if (parse_end_signal(tdev)) {
			tdev->invalid = 1;
			XML_StopParser(tdev->parser, XML_FALSE);
		}
	} else if (!strcmp(name, "tiaMetaInfo"))
		parse_end_tiametainfo(tdev);
}


static
int init_xml_parser(struct tia_eegdev* tdev)
{
	if (!(tdev->parser = XML_ParserCreate("UTF-8")))
		return -1;

	XML_SetUserData(tdev->parser, tdev);
	XML_SetElementHandler(tdev->parser, start_xmlelt, end_xmlelt);

	return 0;
}


/*****************************************************************
 *                  tobiia control communication                 *
 *****************************************************************/
enum protcall {
	TIA_VERSION = 0,
	TIA_METAINFO,
	TIA_DATACONNECTION,
	TIA_STARTDATA,
	TIA_STOPDATA,
	TIA_STATECONNECTION
};

static const char* const prot_request[] = {
	[TIA_VERSION] = "CheckProtocolVersion",
	[TIA_METAINFO] = "GetMetaInfo",
	[TIA_DATACONNECTION] = "GetDataConnection: TCP",
	[TIA_STARTDATA] = "StartDataTransmission",
	[TIA_STOPDATA] = "StopDataTransmission",
	[TIA_STATECONNECTION] = "GetServerStateConnection",
};
static const char* const prot_answer[] = {
	[TIA_VERSION] = "OK",
	[TIA_METAINFO] = "MetaInfo",
	[TIA_DATACONNECTION] = "DataConnectionPort: %i",
	[TIA_STARTDATA] = "OK",
	[TIA_STOPDATA] = "OK",
	[TIA_STATECONNECTION] = "ServerStateConnectionPort: %i",
};


static
int tia_request(struct tia_eegdev* tdev, enum protcall req)
{
	char buffer[64], msg[32];
	void *xmlbuf;
	unsigned int vers[2], clen, len = 0;
	int ret = 0;

	// Send request and read answer message header
	sprintf(buffer, "TiA 1.0\n%s\n\n", prot_request[req]);
	if (egdi_fullwrite(tdev->ctrlfd, buffer, strlen(buffer))
	   || !fgets(buffer, sizeof(buffer), tdev->ctrl)
           || sscanf(buffer, " TiA %u.%u", vers, vers+1) < 2
	   || !fgets(buffer, sizeof(buffer), tdev->ctrl)
           || sscanf(buffer, " %31[^\n]", msg) < 1
	   || !fgets(buffer, sizeof(buffer), tdev->ctrl))
		return -1;

	// Read if additional content is present and skip one line if so
	sscanf(buffer, " Content-Length: %u\n", &len);
	if (len && !fgets(buffer, sizeof(buffer), tdev->ctrl))
			return -1;

	// Read and parse additional XML content if provided
	while (len) {
		clen = (len < XML_BSIZE) ? len : XML_BSIZE;

		// Alloc chunk buffer and fill it
		if ( !(xmlbuf = XML_GetBuffer(tdev->parser, XML_BSIZE))
		  || !fread(xmlbuf, clen, 1, tdev->ctrl)
		  || !XML_ParseBuffer(tdev->parser, clen, !(len-=clen)) )
			return -1;
	}

	// Parse returned message
	switch (req) {
	case TIA_METAINFO:
	case TIA_VERSION:
	case TIA_STARTDATA:
	case TIA_STOPDATA:
		if (strcmp(msg, prot_answer[req]))
			ret = -1;
		break;

	case TIA_DATACONNECTION:
	case TIA_STATECONNECTION:
		if (!sscanf(msg, prot_answer[req], &ret))
			ret = -1;
		break;
	}

	if (tdev->invalid) {
		tdev->invalid = 0;
		ret = -1;
	}
	return ret;
}


static
int init_ctrl_com(struct tia_eegdev* tdev, const char* host, int port)
{
	if ((tdev->ctrlfd = connect_server(host, port)) < 0)
		return -1;
	
	if (!(tdev->ctrl = fdopen(tdev->ctrlfd, "r"))) {
		close(tdev->ctrlfd);
		tdev->ctrlfd = -1;
		return -1;
	}
	
	return 0;
}


/*****************************************************************
 *                     tobiia data communication                 *
 *****************************************************************/
#pragma pack(push, 1)
struct data_hdr {
	uint8_t version;
	uint32_t size;
	uint32_t type_flags;
	uint64_t id;
	uint64_t number;
	uint64_t ts;
};
#pragma pack(pop)

static
unsigned int parse_type_flags(uint32_t flags, const struct tia_eegdev* tdev,
                              int offset[32])
{
	unsigned int i, nsig = 0;
	int tiatype;
	uint32_t mask;
	
	// Estimate number of signal
	for (i=0; i<32; i++) {
		mask = ((uint32_t)1) << i;
		if (flags & mask) {
			nsig++;

			// Retrieve the type of flagged signal
			if ((tiatype = get_tobiia_siginfo_mask(mask)) < 0)
				continue;

			// setup the sample offset according to the type
			// of the signal
			offset[nsig-1] = tdev->offset[tiatype];
		}
	}

	return nsig;
}


static
size_t unpack_datapacket(const struct tia_eegdev* tdev,
                         uint32_t type_flags, const void* pbuf, void* sbuf)
{
	unsigned int i, ich, sig, nsig;
	const uint16_t *numch, *blocksize;
	unsigned int stride = tdev->dev.in_samlen/sizeof(float);
	float* data = sbuf;
	const float* sigb;
	int off[32];

	// Parse type flags and packet pointer accordingly
	nsig = parse_type_flags(type_flags, tdev, off);
	numch = (const uint16_t*)pbuf;
	blocksize = ((const uint16_t*)pbuf) + nsig;
	sigb = (const float*)(((const uint16_t*)pbuf) + 2*nsig);

	// convert array grouped by signal type into an array
	// grouped by samples
	for (sig=0; sig<nsig; sig++) {
		// negative offset means that signal should not be sent
		if (off[sig] < 0) {
			sigb += numch[sig]*blocksize[sig];
			continue;
		}

		for (i=0; i<blocksize[sig]; i++) {
			for (ich=0; ich<numch[sig]; ich++)
				data[i*stride + off[sig] + ich] = sigb[ich];
			sigb += numch[sig];
		}
	}

	return blocksize[0]*stride*sizeof(float);
}

static
void free_buffer(void* data)
{
	free(*((void**)data));
}

static
void* data_fn(void *data)
{
	struct tia_eegdev* tdev = data;
	const struct core_interface* restrict ci = &tdev->dev.ci;
	struct data_hdr hdr;
	size_t blen, pbsize;
	int fd = tdev->datafd;
	void *sbuf = NULL, *pbuf = NULL;

	// Make thread cancellable and register cleanup
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
	pthread_cleanup_push(free_buffer, &pbuf);
	pthread_cleanup_push(free_buffer, &sbuf);

	// Allocate utility packet and sample buffers
	pbsize = tdev->nsig*2*sizeof(uint16_t)
	         + tdev->blocksize*tdev->nch*sizeof(float);
	pbuf = malloc(pbsize);
	sbuf = malloc(tdev->dev.in_samlen*tdev->blocksize);

	while (pbuf && sbuf) {
		// Read packet header
		if (egdi_fullread(fd, &hdr, sizeof(hdr)))
			break;

		// Resize packet buffer if too small
		if (pbsize < hdr.size-sizeof(hdr)) {
			pbsize = hdr.size-sizeof(hdr);
			free(pbuf);
			if (!(pbuf = malloc(pbsize)))
				break;
		}

		// Read packet data
		if (egdi_fullread(fd, pbuf, hdr.size-sizeof(hdr)))
			break;

		// Parse packet and update ringbuffer
		blen = unpack_datapacket(tdev, hdr.type_flags, pbuf, sbuf);
		if (ci->update_ringbuffer(&tdev->dev, sbuf, blen))
			break;
	}

	// We can reach here only if there was an error previously
	ci->report_error(&tdev->dev, errno);

	// unregister cleanup: this will free the packet and sample buffers
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}


static
int init_data_com(struct tia_eegdev* tdev, const char* host)
{
	int port;
	struct eegdev* dev = &tdev->dev;

	if (tia_request(tdev, TIA_METAINFO))
		return -1;

	dev->ci.set_input_samlen(dev, tdev->nch*sizeof(float));

	if ( (port = tia_request(tdev, TIA_DATACONNECTION)) < 0
          || (tdev->datafd = connect_server(host, port)) < 0
	  || pthread_create(&tdev->thid, NULL, data_fn, tdev) ) {
	  	if (tdev->datafd >= 0) {
			close(tdev->datafd);
			tdev->datafd = -1;
		}
		return -1;
	}

	return 0;
}


/******************************************************************
 *                  Init/Destroy TOBIIA device                    *
 ******************************************************************/
static
int tia_open_device(struct eegdev* dev, const char* optv[])
{
	struct tia_eegdev* tdev = get_tia(dev);
	const struct core_interface* restrict ci = &dev->ci;
	unsigned short port = atoi(ci->getopt("port", DEFAULTPORT, optv));
	const char *url = ci->getopt("host", DEFAULTHOST, optv);
	char *devid = NULL, host[strlen(url)+1];

	tdev->datafd = tdev->ctrlfd = -1;

	if ( sock_init_network_system()
	  || parse_url(url, host, &port)
	  || !(devid = malloc(strlen(url)+1))
	  || init_xml_parser(tdev)
	  || init_ctrl_com(tdev, host, port)
	  || init_data_com(tdev, host) ) {
		free(devid);
		sock_cleanup_network_system();
		return -1;
	}
	
	strcpy(devid, url);
	tdev->dev.cap.device_type = tia_device_type;
	tdev->dev.cap.device_id = devid;
	return 0;
}


static
int tia_close_device(struct eegdev* dev)
{
	struct tia_eegdev* tdev = get_tia(dev);
	unsigned int i;

	if (tdev == NULL)
		return -1;

	// Free channels metadata
	for (i=0; i<tdev->nch; i++)
		free(tdev->chmap[i].label);
	free(tdev->chmap);
	free((char*)tdev->dev.cap.device_id);

	// Destroy control connection
	if (tdev->ctrl) {
		sock_shutdown(fileno(tdev->ctrl), SHUT_RDWR);
		fclose(tdev->ctrl);
	}

	// Destroy data connection
	if (tdev->datafd >= 0) {
		pthread_cancel(tdev->thid);
		pthread_join(tdev->thid, NULL);
		close(tdev->datafd);
	}

	// Destroy XML parser
	if (tdev->parser)
  		XML_ParserFree(tdev->parser);
	
	sock_cleanup_network_system();
	return 0;
}


/******************************************************************
 *                  tobiia methods implementation                 *
 ******************************************************************/
static 
int tia_set_channel_groups(struct eegdev* dev, unsigned int ngrp,
					const struct grpconf* grp)
{
	struct tia_eegdev* tdev = get_tia(dev);
	int i, nsel;

	nsel = egdi_split_alloc_chgroups(dev, tdev->chmap, ngrp, grp);
	for (i=0; i<nsel; i++)
		dev->selch[i].bsc = 0;
		
	return (nsel >= 0) ? 0 : -1;
}


static 
void tia_fill_chinfo(const struct eegdev* dev, int stype,
	                     unsigned int ich, struct egd_chinfo* info)
{
	int index;
	struct tia_eegdev* tdev = get_tia(dev);
	const struct signal_information* tsiginfo;
	
	// Find channel mapping
	index = egdi_next_chindex(tdev->chmap, stype, ich);
	tsiginfo = tdev->chmap[index].data;
	
	// Fill channel metadata
	info->label = tdev->chmap[index].label;
	info->isint = tsiginfo->isint;
	info->unit = tsiginfo->unit ? tsiginfo->unit : unknown_field;
	info->transducter = tsiginfo->trans ? tsiginfo->trans:unknown_field;
	info->prefiltering = tsiginfo->filt ? tsiginfo->filt:unknown_field;

	// Guess the scaling information from the integer type
	if (!info->isint) {
		info->dtype = EGD_DOUBLE;
		info->min.valdouble = -262144.0;
		info->max.valdouble = 262143.96875;
	} else {
		info->dtype = EGD_INT32;
		info->min.valint32_t = -8388608;
		info->max.valint32_t = 8388607;
	}
}


static
int tia_start_acq(struct eegdev* dev)
{
	return tia_request(get_tia(dev), TIA_STARTDATA);
}


static
int tia_stop_acq(struct eegdev* dev)
{
	return tia_request(get_tia(dev), TIA_STOPDATA);
}


API_EXPORTED
const struct egdi_plugin_info eegdev_plugin_info = {
	.plugin_abi = 	EEGDEV_PLUGIN_ABI_VERSION,
	.struct_size = 	sizeof(struct tia_eegdev),
	.open_device = 		tia_open_device,
	.close_device = 	tia_close_device,
	.set_channel_groups = 	tia_set_channel_groups,
	.fill_chinfo = 		tia_fill_chinfo,
	.start_acq = 		tia_start_acq,
	.stop_acq = 		tia_stop_acq
};

