/*
	Ralink RT2501 driver for Violet embedded platforms
	(c) 2006 Sebastien Bourdeauducq
*/
extern int gb_encryption;

#include <intrinsics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "usbctrl.h"
#include "mem.h"
#include "hcdmem.h"
#include "hcd.h"
#include "usbh.h"
#include "delay.h"
#include "debug.h"

#include "ieee80211.h"
#include "eapol.h"
#include "rt2501usb_hw.h"
#include "rt2501usb_io.h"
#include "rt2501usb_internal.h"
#include "rt2501usb.h"

const unsigned char ieee80211_broadcast_address[IEEE80211_ADDR_LEN] =
{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
const unsigned char ieee80211_null_address[IEEE80211_ADDR_LEN] =
{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

static const unsigned char ieee80211_vendor_wpa_id[] =
{ 0x00, 0x50, 0xf2, 0x01, 0x01, 0x00 };

static const unsigned char ieee80211_tkip_oui[IEEE80211_OUI_LEN] =
{ 0x00, 0x50, 0xf2, 0x02 };

static const unsigned char ieee80211_psk_oui[IEEE80211_OUI_LEN] =
{ 0x00, 0x50, 0xf2, 0x02 };

int ieee80211_mode;
int ieee80211_state;
unsigned int ieee80211_timeout;
static rt2501_scan_callback ieee80211_scallback;
static void *ieee80211_scallback_userparam;

unsigned char ieee80211_assoc_mac[IEEE80211_ADDR_LEN];
unsigned char ieee80211_assoc_bssid[IEEE80211_ADDR_LEN];
char ieee80211_assoc_ssid[IEEE80211_SSID_MAXLEN+1];
unsigned char ieee80211_assoc_channel;
unsigned short int ieee80211_assoc_rateset;

static short int ieee80211_rssi_samples[RT2501_RSSI_SAMPLES];
static short int ieee80211_rssi_sample_index;
static short int ieee80211_rssi_average;

unsigned char ieee80211_authmode;
unsigned char ieee80211_encryption;
unsigned char ieee80211_key[IEEE80211_MAX_KEYLEN];


static struct ieee80211_sta_state ieee80211_associated_sta[RT2501_MAX_ASSOCIATED_STA];

/* IEEE80211_RATEMASK_* */
/* filled at auth with the lowest TX rate, updated according to RSSI when in RUN state and Managed mode */
/* also filled when entering Master mode */
static unsigned short int ieee80211_txrate;
/* filled at auth and when entering Master mode, does not change */
static unsigned short int ieee80211_lowest_txrate;

static unsigned short int ieee80211_rate_to_mask(unsigned char rate)
{
	switch(rate) {
		case 2:
			return IEEE80211_RATEMASK_1;
		case 4:
			return IEEE80211_RATEMASK_2;
		case 11:
			return IEEE80211_RATEMASK_5_5;
		case 22:
			return IEEE80211_RATEMASK_11;
			
		case 12:
			return IEEE80211_RATEMASK_6;
		case 18:
			return IEEE80211_RATEMASK_9;
		case 24:
			return IEEE80211_RATEMASK_12;
		case 36:
			return IEEE80211_RATEMASK_18;
		case 48:
			return IEEE80211_RATEMASK_24;
		case 72:
			return IEEE80211_RATEMASK_36;
		case 96:
			return IEEE80211_RATEMASK_48;
		case 108:
			return IEEE80211_RATEMASK_54;
		default:
#ifdef DEBUG_WIFI
			sprintf(dbg_buffer,
				"Unknown rate in ieee80211_rate_to_mask (%d)\r\n",
				rate);
			DBG_WIFI(dbg_buffer);
#endif
			return 0;
	}
}

static unsigned char ieee80211_mask_to_rate(unsigned short int mask)
{
	switch(mask) {
		case IEEE80211_RATEMASK_1:
			return 2;
		case IEEE80211_RATEMASK_2:
			return 4;
		case IEEE80211_RATEMASK_5_5:
			return 11;
		case IEEE80211_RATEMASK_11:
			return 22;
		case IEEE80211_RATEMASK_6:
			return 12;
		case IEEE80211_RATEMASK_9:
			return 18;
		case IEEE80211_RATEMASK_12:
			return 24;
		case IEEE80211_RATEMASK_18:
			return 36;
		case IEEE80211_RATEMASK_24:
			return 48;
		case IEEE80211_RATEMASK_36:
			return 72;
		case IEEE80211_RATEMASK_48:
			return 96;
		case IEEE80211_RATEMASK_54:
			return 108;
		default:
			DBG_WIFI("Unknown mask in ieee80211_mask_to_rate\r\n");
			return 0;
	}
}

static unsigned char ieee80211_mask_to_rt2501rate(unsigned short int mask)
{
	switch(mask) {
		case IEEE80211_RATEMASK_1:
			return RT2501_RATE_1;
		case IEEE80211_RATEMASK_2:
			return RT2501_RATE_2;
		case IEEE80211_RATEMASK_5_5:
			return RT2501_RATE_5_5;
		case IEEE80211_RATEMASK_11:
			return RT2501_RATE_11;
		case IEEE80211_RATEMASK_6:
			return RT2501_RATE_6;
		case IEEE80211_RATEMASK_9:
			return RT2501_RATE_9;
		case IEEE80211_RATEMASK_24:
			return RT2501_RATE_24;
		case IEEE80211_RATEMASK_36:
			return RT2501_RATE_36;
		case IEEE80211_RATEMASK_48:
			return RT2501_RATE_48;
		case IEEE80211_RATEMASK_54:
			return RT2501_RATE_54;
		default:
			return RT2501_RATE_1;
	}
}

static unsigned short int ieee80211_mask_to_rt2501mask(unsigned short ieee80211mask)
{
	unsigned short int rt2501mask;

	rt2501mask = 0;
	if(ieee80211mask & IEEE80211_RATEMASK_1) rt2501mask |= RT2501_RATEMASK_1;
	if(ieee80211mask & IEEE80211_RATEMASK_2) rt2501mask |= RT2501_RATEMASK_2;
	if(ieee80211mask & IEEE80211_RATEMASK_5_5) rt2501mask |= RT2501_RATEMASK_5_5;
	if(ieee80211mask & IEEE80211_RATEMASK_6) rt2501mask |= RT2501_RATEMASK_6;
	if(ieee80211mask & IEEE80211_RATEMASK_9) rt2501mask |= RT2501_RATEMASK_9;
	if(ieee80211mask & IEEE80211_RATEMASK_11) rt2501mask |= RT2501_RATEMASK_11;
	if(ieee80211mask & IEEE80211_RATEMASK_12) rt2501mask |= RT2501_RATEMASK_12;
	if(ieee80211mask & IEEE80211_RATEMASK_18) rt2501mask |= RT2501_RATEMASK_18;
	if(ieee80211mask & IEEE80211_RATEMASK_24) rt2501mask |= RT2501_RATEMASK_24;
	if(ieee80211mask & IEEE80211_RATEMASK_36) rt2501mask |= RT2501_RATEMASK_36;
	if(ieee80211mask & IEEE80211_RATEMASK_48) rt2501mask |= RT2501_RATEMASK_48;
	if(ieee80211mask & IEEE80211_RATEMASK_54) rt2501mask |= RT2501_RATEMASK_54;

	return rt2501mask;
}

static unsigned char ieee80211_crypt_to_rt2501cipher(unsigned char crypt)
{
	switch(crypt) {
		case IEEE80211_CRYPT_NONE:
			return RT2501_CIPHER_NONE;
		case IEEE80211_CRYPT_WEP64:
			return RT2501_CIPHER_WEP64;
		case IEEE80211_CRYPT_WEP128:
			return RT2501_CIPHER_WEP128;
		case IEEE80211_CRYPT_WPA:
			return RT2501_CIPHER_TKIP;
		default:
			return RT2501_CIPHER_NONE;
	}
}

/* IEEE80211_RATEMASK_* */
static unsigned short int ieee80211_find_closest_rate(unsigned short int wanted_rate)
{
	unsigned short int i;

	i = wanted_rate;
	while(!(ieee80211_assoc_rateset & i) && (i != 1)) i = (i >> 1);
	if(ieee80211_assoc_rateset & i) return i;

	if(ieee80211_assoc_rateset == 0) return IEEE80211_RATEMASK_1;
	i = 1;
	/* always terminates because ieee80211_assoc_rateset != 0 */
	while(!(ieee80211_assoc_rateset & i)) i = (i << 1);
	return i;
}

static void ieee80211_new_rssi_sample(short int rssi)
{
	ieee80211_rssi_samples[ieee80211_rssi_sample_index++] = rssi;
	if(ieee80211_rssi_sample_index == RT2501_RSSI_SAMPLES) {
		unsigned int i;
		int a;
		unsigned short int wanted_txrate;

		a = 0;
		for(i=0;i<RT2501_RSSI_SAMPLES;i++)
			a += ieee80211_rssi_samples[i];
		ieee80211_rssi_average = a/RT2501_RSSI_SAMPLES;
		ieee80211_rssi_sample_index = 0;

		/* Update TX rate */
		if(ieee80211_rssi_average >= -65)
			wanted_txrate = IEEE80211_RATEMASK_54;
		else if(ieee80211_rssi_average >= -66)
			wanted_txrate = IEEE80211_RATEMASK_48;
		else if(ieee80211_rssi_average >= -70)
			wanted_txrate = IEEE80211_RATEMASK_36;
		else if(ieee80211_rssi_average >= -74)
			wanted_txrate = IEEE80211_RATEMASK_24;
		else if(ieee80211_rssi_average >= -77)
			wanted_txrate = IEEE80211_RATEMASK_18;
		else if(ieee80211_rssi_average >= -79)
			wanted_txrate = IEEE80211_RATEMASK_12;
		else if(ieee80211_rssi_average >= -81)
			wanted_txrate = IEEE80211_RATEMASK_11;
		else if(ieee80211_rssi_average >= -84)
			wanted_txrate = IEEE80211_RATEMASK_5_5;
		else if(ieee80211_rssi_average >= -85)
			wanted_txrate = IEEE80211_RATEMASK_2;
		else
			wanted_txrate = IEEE80211_RATEMASK_1;
		ieee80211_txrate = ieee80211_find_closest_rate(wanted_txrate);
	}
}

static int ieee80211_find_sta_slot_auth(unsigned char *sta_mac)
{
	int i;

	/*
	Is the MAC address of the station already in the table ?
	It can happen if the station is retransmitting its auth because it
	failed to receive our reply.
	*/
	for(i=0;i<RT2501_MAX_ASSOCIATED_STA;i++) {
		if((ieee80211_associated_sta[i].state != IEEE80211_S_IDLE)
				  && (memcmp(ieee80211_associated_sta[i].mac, sta_mac,
				      IEEE80211_ADDR_LEN) == 0)) return i;
	}
	/* No, search for an empty slot */
	for(i=0;i<RT2501_MAX_ASSOCIATED_STA;i++) {
		if(ieee80211_associated_sta[i].state == IEEE80211_S_IDLE)
			return i;
	}
	/* No slot found ! */
	return -1;
}

static int ieee80211_find_sta_slot(unsigned char *sta_mac)
{
	int i;

	for(i=0;i<RT2501_MAX_ASSOCIATED_STA;i++) {
		if((ieee80211_associated_sta[i].state != IEEE80211_S_IDLE)
				  && (memcmp(ieee80211_associated_sta[i].mac, sta_mac,
				      IEEE80211_ADDR_LEN) == 0)) return i;
	}
	return -1;
}

static void ieee80211_send_auth(unsigned char *destination_mac,
				unsigned short int algorithm,
				unsigned short int auth_seq,
				unsigned short int status)
{
#pragma pack(1)
	struct {
	TXD_STRUC txd;
	struct ieee80211_frame header;
	char auth[6];
	} *auth;
#pragma pack()
	unsigned short int duration;

#ifdef DEBUG_WIFI
	sprintf(dbg_buffer, "Sending auth to %02x:%02x:%02x:%02x:%02x:%02x, status %d\r\n",
		destination_mac[0], destination_mac[1], destination_mac[2],
		destination_mac[3], destination_mac[4], destination_mac[5],
		status);
	DBG_WIFI(dbg_buffer);
#endif

	disable_ohci_irq();
	auth = hcd_malloc(sizeof(*auth)+7, COMRAM,8);
	enable_ohci_irq();
	if(auth == NULL) {
		DBG_WIFI("hcd_malloc failed in ieee80211_send_auth\r\n");
		return;
	}
	auth->header.i_fc[0] = IEEE80211_FC0_TYPE_MGT|IEEE80211_FC0_SUBTYPE_AUTH
			|(IEEE80211_FC0_VERSION_0 << IEEE80211_FC0_VERSION_SHIFT);
	auth->header.i_fc[1] = IEEE80211_FC1_DIR_NODS;
	duration = rt2501_txtime(sizeof(*auth)-sizeof(TXD_STRUC), ieee80211_mask_to_rate(ieee80211_lowest_txrate))+IEEE80211_SIFS;
	auth->header.i_dur[0] = ((duration & 0x00ff) >> 0);
	auth->header.i_dur[1] = ((duration & 0xff00) >> 8);
	memcpy(auth->header.i_addr1, destination_mac, IEEE80211_ADDR_LEN);
	memcpy(auth->header.i_addr2, rt2501_mac, IEEE80211_ADDR_LEN);
	memcpy(auth->header.i_addr3, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN);
	auth->header.i_seq[0] = 0;
	auth->header.i_seq[1] = 0;

	/* algo[2], seq[2], status[2] */
	auth->auth[0] = ((algorithm & 0x00ff) >> 0);
	auth->auth[1] = ((algorithm & 0xff00) >> 8);
	auth->auth[2] = ((auth_seq & 0x00ff) >> 0);
	auth->auth[3] = ((auth_seq & 0xff00) >> 8);
	auth->auth[4] = ((status & 0x00ff) >> 0);
	auth->auth[5] = ((status & 0xff00) >> 8);

	rt2501_make_tx_descriptor(
	&auth->txd,
	RT2501_CIPHER_NONE,			/* CipherAlg */
	0,					/* KeyTable */
	0,					/* KeyIdx */
	1,					/* Ack */
	0,					/* Fragment */
	0,					/* InsTimestamp */
	1,					/* RetryMode */
	0,					/* Ifs */
	/* Rate */
	ieee80211_mask_to_rt2501rate(ieee80211_lowest_txrate),
	sizeof(*auth)-sizeof(TXD_STRUC),	/* Length */
	0,					/* QueIdx */
	0					/* PacketId */
				 );

	if(!rt2501_tx(auth, sizeof(*auth)))
		DBG_WIFI("TX error in ieee80211_send_auth\r\n");
}

static void ieee80211_send_assocresp(unsigned char *sta_mac, unsigned short int status,
				     unsigned short int assoc_id)
{
#pragma pack(1)
	struct {
	TXD_STRUC txd;
	struct ieee80211_frame header;
	char assoc[6+2+8+2+4];
	} *assoc;
#pragma pack()
	char *write_ptr;
	unsigned int frame_length;
	unsigned short int duration;

#ifdef DEBUG_WIFI
	sprintf(dbg_buffer, "Sending assoc reply, status %d\r\n", status);
	DBG_WIFI(dbg_buffer);
#endif

	disable_ohci_irq();
	assoc = hcd_malloc(sizeof(*assoc)+7, COMRAM,9);
	enable_ohci_irq();
	if(assoc == NULL) {
		DBG_WIFI("hcd_malloc failed in ieee80211_send_assocresp\r\n");
		return ;
	}

	assoc->header.i_fc[0] = IEEE80211_FC0_TYPE_MGT|IEEE80211_FC0_SUBTYPE_ASSOC_RESP
			|(IEEE80211_FC0_VERSION_0 << IEEE80211_FC0_VERSION_SHIFT);
	assoc->header.i_fc[1] = IEEE80211_FC1_DIR_NODS;
	memcpy(assoc->header.i_addr1, sta_mac, IEEE80211_ADDR_LEN);
	memcpy(assoc->header.i_addr2, rt2501_mac, IEEE80211_ADDR_LEN);
	memcpy(assoc->header.i_addr3, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN);
	assoc->header.i_seq[0] = 0;
	assoc->header.i_seq[1] = 0;

	write_ptr = assoc->assoc;
	/* FIXED PARAMETERS */
	*(write_ptr++) = (IEEE80211_CAPINFO_ESS & 0x00ff) >> 0;
	*(write_ptr++) = (IEEE80211_CAPINFO_ESS & 0xff00) >> 8;
	*(write_ptr++) = (status & 0x00ff) >> 0;
	*(write_ptr++) = (status & 0xff00) >> 8;
	*(write_ptr++) = (assoc_id & 0x00ff) >> 0;
	*(write_ptr++) = (assoc_id & 0xff00) >> 8;
	/* TAGGED PARAMETERS */
	/* Supported Rates */
	*(write_ptr++) = IEEE80211_ELEMID_RATES;
	*(write_ptr++) = 8;
	*(write_ptr++) = 0x02;
	*(write_ptr++) = 0x04;
	*(write_ptr++) = 0x0b;
	*(write_ptr++) = 0x16;
	*(write_ptr++) = 0x24;
	*(write_ptr++) = 0x30;
	*(write_ptr++) = 0x48;
	*(write_ptr++) = 0x6c;
	/* Extended Supported Rates */
	*(write_ptr++) = IEEE80211_ELEMID_XRATES;
	*(write_ptr++) = 4;
	*(write_ptr++) = 0x0c;
	*(write_ptr++) = 0x12;
	*(write_ptr++) = 0x18;
	*(write_ptr++) = 0x60;

	frame_length = sizeof(struct ieee80211_frame)+((unsigned int)write_ptr - (unsigned int)assoc->assoc);

	duration = rt2501_txtime(frame_length, ieee80211_mask_to_rate(ieee80211_lowest_txrate))+IEEE80211_SIFS;
	assoc->header.i_dur[0] = ((duration & 0x00ff) >> 0);
	assoc->header.i_dur[1] = ((duration & 0xff00) >> 8);

	rt2501_make_tx_descriptor(
	&assoc->txd,
	RT2501_CIPHER_NONE,		/* CipherAlg */
	0,				/* KeyTable */
	0,				/* KeyIdx */
	1,				/* Ack */
	0,				/* Fragment */
	0,				/* InsTimestamp */
	1,				/* RetryMode */
	0,				/* Ifs */
	/* Rate */
	ieee80211_mask_to_rt2501rate(ieee80211_lowest_txrate),
	frame_length,			/* Length */
	0,				/* QueIdx */
	0				/* PacketId */
				 );

	if(!rt2501_tx(assoc, frame_length+sizeof(TXD_STRUC)))
		DBG_WIFI("TX error in ieee80211_send_assoc\r\n");
}

static void ieee80211_auth_sta(unsigned char *sta_mac, unsigned short int algorithm,
			       unsigned short int auth_seq,
			       unsigned short int status)
{
	int sta_slot;

	if(algorithm != IEEE80211_AUTH_ALG_OPEN) {
		ieee80211_send_auth(sta_mac, algorithm, auth_seq+1, IEEE80211_STATUS_ALG);
		return;
	}
	if(auth_seq != IEEE80211_AUTH_OPEN_REQUEST) {
		ieee80211_send_auth(sta_mac, algorithm, auth_seq+1, IEEE80211_STATUS_SEQUENCE);
		return;
	}
	if(status != IEEE80211_STATUS_SUCCESS) {
		ieee80211_send_auth(sta_mac, algorithm, auth_seq+1, IEEE80211_STATUS_OTHER);
		return;
	}

	/*
	Find an empty slot, or a slot that has the MAC address of the station
	(see comments above).
	*/
	sta_slot = ieee80211_find_sta_slot_auth(sta_mac);
	if(sta_slot == -1) {
		ieee80211_send_auth(sta_mac,
				    IEEE80211_AUTH_ALG_OPEN,
				    IEEE80211_AUTH_OPEN_RESPONSE,
				    IEEE80211_STATUS_TOO_MANY_STATIONS);
		return;
	}
	ieee80211_associated_sta[sta_slot].state = IEEE80211_S_ASSOC;
	ieee80211_associated_sta[sta_slot].timer = IEEE80211_STA_ASSOC_TIMEOUT;
	memcpy(ieee80211_associated_sta[sta_slot].mac, sta_mac, IEEE80211_ADDR_LEN);
	ieee80211_send_auth(sta_mac,
			    IEEE80211_AUTH_ALG_OPEN,
			    IEEE80211_AUTH_OPEN_RESPONSE,
			    IEEE80211_STATUS_SUCCESS);
}

static void ieee80211_deauth_sta(int index, unsigned short int reason)
{
#pragma pack(1)
	struct {
	TXD_STRUC txd;
	struct ieee80211_frame header;
	char deauth[2];
	} *deauth;
#pragma pack()
	unsigned short int duration;

#ifdef DEBUG_WIFI
	sprintf(dbg_buffer, "Deauthenticating %02x:%02x:%02x:%02x:%02x:%02x\r\n",
		ieee80211_associated_sta[index].mac[0],
		ieee80211_associated_sta[index].mac[1],
		ieee80211_associated_sta[index].mac[2],
		ieee80211_associated_sta[index].mac[3],
		ieee80211_associated_sta[index].mac[4],
		ieee80211_associated_sta[index].mac[5]);
	DBG_WIFI(dbg_buffer);
#endif

	ieee80211_associated_sta[index].state = IEEE80211_S_IDLE;

	disable_ohci_irq();
	deauth = hcd_malloc(sizeof(*deauth)+7, COMRAM,10);
	enable_ohci_irq();
	if(deauth == NULL) {
		DBG_WIFI("hcd_malloc failed in ieee80211_deauth_sta\r\n");
		return;
	}
	deauth->header.i_fc[0] = IEEE80211_FC0_TYPE_MGT|IEEE80211_FC0_SUBTYPE_DEAUTH
			|(IEEE80211_FC0_VERSION_0 << IEEE80211_FC0_VERSION_SHIFT);
	deauth->header.i_fc[1] = IEEE80211_FC1_DIR_NODS;
	duration = rt2501_txtime(sizeof(*deauth)-sizeof(TXD_STRUC), ieee80211_mask_to_rate(ieee80211_lowest_txrate))+IEEE80211_SIFS;
	deauth->header.i_dur[0] = ((duration & 0x00ff) >> 0);
	deauth->header.i_dur[1] = ((duration & 0xff00) >> 8);
	memcpy(deauth->header.i_addr1, ieee80211_associated_sta[index].mac, IEEE80211_ADDR_LEN);
	memcpy(deauth->header.i_addr2, rt2501_mac, IEEE80211_ADDR_LEN);
	memcpy(deauth->header.i_addr3, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN);
	deauth->header.i_seq[0] = 0;
	deauth->header.i_seq[1] = 0;

	deauth->deauth[0] = ((reason & 0x00ff) >> 0);
	deauth->deauth[1] = ((reason & 0xff00) >> 8);

	rt2501_make_tx_descriptor(
	&deauth->txd,
	RT2501_CIPHER_NONE,			/* CipherAlg */
	0,					/* KeyTable */
	0,					/* KeyIdx */
	1,					/* Ack */
	0,					/* Fragment */
	0,					/* InsTimestamp */
	1,					/* RetryMode */
	0,					/* Ifs */
	/* Rate */
	ieee80211_mask_to_rt2501rate(ieee80211_lowest_txrate),
	sizeof(*deauth)-sizeof(TXD_STRUC),	/* Length */
	0,					/* QueIdx */
	0					/* PacketId */
				 );

	if(!rt2501_tx(deauth, sizeof(*deauth)))
		DBG_WIFI("TX error in ieee80211_deauth_sta\r\n");
}

static void ieee80211_associate(void)
{
#pragma pack(1)
	struct {
	TXD_STRUC txd;
	struct ieee80211_frame header;
	char assoc[4
		+2+IEEE80211_SSID_MAXLEN
		+2+8
		+2+4
		+2+22];
	} *assoc;
#pragma pack()
	char *write_ptr;
	unsigned int i, j;
	unsigned int frame_length;
	unsigned short int duration;

	DBG_WIFI("Associating\r\n");

	disable_ohci_irq();
	assoc = hcd_malloc(sizeof(*assoc)+7, COMRAM,11);
	enable_ohci_irq();
	if(assoc == NULL) {
		DBG_WIFI("hcd_malloc failed in ieee80211_associate\r\n");
		return ;
	}

	assoc->header.i_fc[0] = IEEE80211_FC0_TYPE_MGT|IEEE80211_FC0_SUBTYPE_ASSOC_REQ
			|(IEEE80211_FC0_VERSION_0 << IEEE80211_FC0_VERSION_SHIFT);
	assoc->header.i_fc[1] = IEEE80211_FC1_DIR_NODS;
	memcpy(assoc->header.i_addr1, ieee80211_assoc_mac, IEEE80211_ADDR_LEN);
	memcpy(assoc->header.i_addr2, rt2501_mac, IEEE80211_ADDR_LEN);
	memcpy(assoc->header.i_addr3, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN);
	assoc->header.i_seq[0] = 0;
	assoc->header.i_seq[1] = 0;

	write_ptr = assoc->assoc;
	/* FIXED PARAMETERS */
	/* Capability Information */
	*(write_ptr++) = ((IEEE80211_CAPINFO_ESS & 0x00ff) >> 0);
	*(write_ptr++) = ((IEEE80211_CAPINFO_ESS & 0xff00) >> 8);
	/* Listen Interval */
	*(write_ptr++) = 0x64;
	*(write_ptr++) = 0x00;
	/* TAGGED PARAMETERS */
	/* SSID */
	*(write_ptr++) = IEEE80211_ELEMID_SSID;
	j = strlen(ieee80211_assoc_ssid);
	*(write_ptr++) = j;
	for(i=0;i<j;i++)
		*(write_ptr++) = ieee80211_assoc_ssid[i];
	/* Supported Rates */
	*(write_ptr++) = IEEE80211_ELEMID_RATES;
	*(write_ptr++) = 8;
	*(write_ptr++) = 0x02;
	*(write_ptr++) = 0x04;
	*(write_ptr++) = 0x0b;
	*(write_ptr++) = 0x16;
	*(write_ptr++) = 0x24;
	*(write_ptr++) = 0x30;
	*(write_ptr++) = 0x48;
	*(write_ptr++) = 0x6c;
	/* Extended Supported Rates */
	*(write_ptr++) = IEEE80211_ELEMID_XRATES;
	*(write_ptr++) = 4;
	*(write_ptr++) = 0x0c;
	*(write_ptr++) = 0x12;
	*(write_ptr++) = 0x18;
	*(write_ptr++) = 0x60;
	
	if(ieee80211_encryption == IEEE80211_CRYPT_WPA) {
		*(write_ptr++) = IEEE80211_ELEMID_VENDOR;
		*(write_ptr++) = 22;
		for(i=0;i<sizeof(ieee80211_vendor_wpa_id);i++)
			*(write_ptr++) = ieee80211_vendor_wpa_id[i];
		/* Multicast cipher suite: TKIP */
		for(i=0;i<IEEE80211_OUI_LEN;i++)
			*(write_ptr++) = ieee80211_tkip_oui[i];
		/* 1 unicast cipher suite */
		*(write_ptr++) = 0x01;
		*(write_ptr++) = 0x00;
		/* Unicast cipher suites: TKIP */
		for(i=0;i<IEEE80211_OUI_LEN;i++)
			*(write_ptr++) = ieee80211_tkip_oui[i];
		/* 1 auth key management suite */
		*(write_ptr++) = 0x01;
		*(write_ptr++) = 0x00;
		/* Auth key management suites: PSK */
		for(i=0;i<IEEE80211_OUI_LEN;i++)
			*(write_ptr++) = ieee80211_psk_oui[i];

//
//rsn;
                if (gb_encryption==1) {
              	DBG_WIFI("add rsn frame\r\n");
		*(write_ptr++) = 0x14;
		*(write_ptr++) = 0x01;
		*(write_ptr++) = 0x00;
		*(write_ptr++) = 0x00;
		*(write_ptr++) = 0x0f;
		*(write_ptr++) = 0xac;
		*(write_ptr++) = 0x04;
		*(write_ptr++) = 0x01;
		*(write_ptr++) = 0x00;
		*(write_ptr++) = 0x00;
		*(write_ptr++) = 0x0f;
		*(write_ptr++) = 0xac;
		*(write_ptr++) = 0x04;
		*(write_ptr++) = 0x01;
		*(write_ptr++) = 0x00;
		*(write_ptr++) = 0x00;
		*(write_ptr++) = 0x0f;
		*(write_ptr++) = 0xac;
		*(write_ptr++) = 0x02;
		*(write_ptr++) = 0x3c;
		*(write_ptr++) = 0x00;
                }
//

	}

	frame_length = sizeof(struct ieee80211_frame)+(write_ptr - assoc->assoc);

	duration = rt2501_txtime(frame_length, ieee80211_mask_to_rate(ieee80211_lowest_txrate))+IEEE80211_SIFS;
	assoc->header.i_dur[0] = ((duration & 0x00ff) >> 0);
	assoc->header.i_dur[1] = ((duration & 0xff00) >> 8);

	rt2501_make_tx_descriptor(
	&assoc->txd,
	RT2501_CIPHER_NONE,		/* CipherAlg */
	0,				/* KeyTable */
	0,				/* KeyIdx */
	1,				/* Ack */
	0,				/* Fragment */
	0,				/* InsTimestamp */
	1,				/* RetryMode */
	0,				/* Ifs */
	/* Rate */
	ieee80211_mask_to_rt2501rate(ieee80211_lowest_txrate),
	frame_length,			/* Length */
	0,				/* QueIdx */
	0				/* PacketId */
				 );

	if(!rt2501_tx(assoc, sizeof(TXD_STRUC)+frame_length)) {
		DBG_WIFI("TX error in ieee80211_associate\r\n");
		ieee80211_state = IEEE80211_S_IDLE;
		return;
	}

	ieee80211_state = IEEE80211_S_ASSOC;
	ieee80211_timeout = IEEE80211_ASSOC_TIMEOUT;
}

static void ieee80211_send_challenge_reply(char *challenge, unsigned int challenge_length)
{
#pragma pack(1)
	struct {
	TXD_STRUC txd;
	struct ieee80211_frame header;
	char auth[6+2+IEEE80211_CHALLENGE_LEN];
	} *auth;
#pragma pack()
	unsigned short int duration;
	char *write_ptr;
	unsigned int i;
	unsigned int frame_length;

	DBG_WIFI("Replying to challenge\r\n");
	if(challenge_length != IEEE80211_CHALLENGE_LEN) {
		DBG_WIFI("Incorrect challenge received (length)\r\n");
		return;
	}

	disable_ohci_irq();
	auth = hcd_malloc(sizeof(*auth)+7, COMRAM,12);
	enable_ohci_irq();
	if(auth == NULL) {
		DBG_WIFI("hcd_malloc failed in ieee80211_send_challenge_reply\r\n");
		return ;
	}

	auth->header.i_fc[0] = IEEE80211_FC0_TYPE_MGT|IEEE80211_FC0_SUBTYPE_AUTH
			|(IEEE80211_FC0_VERSION_0 << IEEE80211_FC0_VERSION_SHIFT);
	auth->header.i_fc[1] = IEEE80211_FC1_DIR_NODS|IEEE80211_FC1_PROTECTED;
	memcpy(auth->header.i_addr1, ieee80211_assoc_mac, IEEE80211_ADDR_LEN);
	memcpy(auth->header.i_addr2, rt2501_mac, IEEE80211_ADDR_LEN);
	memcpy(auth->header.i_addr3, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN);
	auth->header.i_seq[0] = 0;
	auth->header.i_seq[1] = 0;

	write_ptr = auth->auth;
	/* FIXED PARAMETERS */
	*(write_ptr++) = ((IEEE80211_AUTH_ALG_SHARED & 0x00ff) >> 0);
	*(write_ptr++) = ((IEEE80211_AUTH_ALG_SHARED & 0xff00) >> 8);
	*(write_ptr++) = ((IEEE80211_AUTH_SHARED_RESPONSE & 0x00ff) >> 0);
	*(write_ptr++) = ((IEEE80211_AUTH_SHARED_RESPONSE & 0xff00) >> 8);
	*(write_ptr++) = ((IEEE80211_STATUS_SUCCESS & 0x00ff) >> 0);
	*(write_ptr++) = ((IEEE80211_STATUS_SUCCESS & 0xff00) >> 8);
	/* TAGGED PARAMETERS */
	/* Challenge */
	*(write_ptr++) = IEEE80211_ELEMID_CHALLENGE;
	*(write_ptr++) = IEEE80211_CHALLENGE_LEN;
	for(i=0;i<IEEE80211_CHALLENGE_LEN;i++)
		*(write_ptr++) = challenge[i];

	frame_length = sizeof(struct ieee80211_frame)+(write_ptr - auth->auth);

	duration = rt2501_txtime(frame_length, ieee80211_mask_to_rate(ieee80211_lowest_txrate))+IEEE80211_SIFS;
	auth->header.i_dur[0] = ((duration & 0x00ff) >> 0);
	auth->header.i_dur[1] = ((duration & 0xff00) >> 8);

	rt2501_make_tx_descriptor(
	&auth->txd,
	/* CipherAlg */
	ieee80211_crypt_to_rt2501cipher(ieee80211_encryption),
	0,				/* KeyTable */
	0,				/* KeyIdx */
	1,				/* Ack */
	0,				/* Fragment */
	0,				/* InsTimestamp */
	1,				/* RetryMode */
	0,				/* Ifs */
	/* Rate */
	ieee80211_mask_to_rt2501rate(ieee80211_lowest_txrate),
	frame_length,			/* Length */
	0,				/* QueIdx */
	0				/* PacketId */
				 );
	auth->txd.Iv = rand() & 0x00ffffff;
	auth->txd.IvOffset = sizeof(struct ieee80211_frame);

#ifdef DEBUG_WIFI
	sprintf(dbg_buffer, "IV = 0x%08x, offset 0x%04x\r\n", auth->txd.Iv, auth->txd.IvOffset);
	DBG_WIFI(dbg_buffer);
#endif

	if(!rt2501_tx(auth, sizeof(TXD_STRUC)+frame_length)) {
		DBG_WIFI("TX error in ieee80211_send_challenge_reply\r\n");
		ieee80211_state = IEEE80211_S_IDLE;
		return;
	}
}

static void ieee80211_input_shared_auth(unsigned short int algorithm,
					unsigned short int auth_seq,
					unsigned short int status,
					char *tagged_parameters, unsigned int tagged_length)
{
	if(algorithm != IEEE80211_AUTH_ALG_SHARED) {
		DBG_WIFI("Shared auth failed: unexpected algo reply\r\n");
		ieee80211_state = IEEE80211_S_IDLE;
		return;
	}
	if(status != IEEE80211_STATUS_SUCCESS) {
		DBG_WIFI("Shared auth failed: denied by AP\r\n");
		ieee80211_state = IEEE80211_S_IDLE;
		return;
	}
	switch(auth_seq) {
		case IEEE80211_AUTH_SHARED_CHALLENGE: {
			char *frame_current, *frame_end;
			char *challenge;
			unsigned char challenge_length;

			DBG_WIFI("Received challenge\r\n");

			challenge = NULL;
			challenge_length = NULL;
			frame_current = tagged_parameters;
			frame_end = tagged_parameters + tagged_length;

			while(frame_current < frame_end) {
				if(frame_current[0] == IEEE80211_ELEMID_CHALLENGE) {
					challenge = &frame_current[2];
					challenge_length = frame_current[1];
				}
				frame_current += (frame_current[1] + 2);
			}

			if((challenge == NULL) || (challenge_length == 0)) {
				DBG_WIFI("Shared auth failed: tagged parameters do not contain challenge\r\n");
				ieee80211_state = IEEE80211_S_IDLE;
				break;
			}

			ieee80211_send_challenge_reply(challenge, challenge_length);

			break;
		}
		case IEEE80211_AUTH_SHARED_PASS:
			DBG_WIFI("Shared auth OK !\r\n");
			ieee80211_associate();
			break;
		default:
			DBG_WIFI("Shared auth failed: unexpected sequence reply\r\n");
			ieee80211_state = IEEE80211_S_IDLE;
			break;
	}
}

static void ieee80211_send_probe_response(unsigned char *dest_mac)
{
#pragma pack(1)
	struct {
		TXD_STRUC txd;
		struct ieee80211_frame header;
		char presp[4 				/* Fixed Parameters */
			+2+IEEE80211_SSID_MAXLEN 	/* SSID */
			+2+8				/* Rates */
			+2+4				/* Extended rates */
			+2+1];				/* Channel */
	} *presp;
#pragma pack()
	char *write_ptr;
	unsigned int i, j;
	unsigned int frame_length;
	unsigned short int duration;

#ifdef DEBUG_WIFI
	sprintf(dbg_buffer, "Sending probe response to %02x:%02x:%02x:%02x:%02x:%02x\r\n",
		dest_mac[0], dest_mac[1], dest_mac[2], dest_mac[3], dest_mac[4], dest_mac[5]);
	DBG_WIFI(dbg_buffer);
#endif

	disable_ohci_irq();
	presp = hcd_malloc(sizeof(*presp)+7, COMRAM,13);
	enable_ohci_irq();
	if(presp == NULL) {
		DBG_WIFI("hcd_malloc failed in ieee80211_send_probe_response\r\n");
		return ;
	}
	
	presp->header.i_fc[0] = IEEE80211_FC0_TYPE_MGT|IEEE80211_FC0_SUBTYPE_PROBE_RESP
			|(IEEE80211_FC0_VERSION_0 << IEEE80211_FC0_VERSION_SHIFT);
	presp->header.i_fc[1] = IEEE80211_FC1_DIR_NODS;
	memcpy(presp->header.i_addr1, dest_mac, IEEE80211_ADDR_LEN);
	memcpy(presp->header.i_addr2, rt2501_mac, IEEE80211_ADDR_LEN);
	memcpy(presp->header.i_addr3, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN);
	presp->header.i_seq[0] = 0;
	presp->header.i_seq[1] = 0;

	write_ptr = presp->presp;
	/* FIXED PARAMETERS */
	/* Timestamp, handled by Ralink chip */
	*(write_ptr++) = 0x00;
	*(write_ptr++) = 0x00;
	*(write_ptr++) = 0x00;
	*(write_ptr++) = 0x00;
	*(write_ptr++) = 0x00;
	*(write_ptr++) = 0x00;
	*(write_ptr++) = 0x00;
	*(write_ptr++) = 0x00;
	/* Beacon Interval */
	*(write_ptr++) = 0x64;
	*(write_ptr++) = 0x00;
	/* Capability Information */
	*(write_ptr++) = ((IEEE80211_CAPINFO_ESS & 0x00ff) >> 0);
	*(write_ptr++) = ((IEEE80211_CAPINFO_ESS & 0xff00) >> 8);
	/* TAGGED PARAMETERS */
	/* SSID */
	*(write_ptr++) = IEEE80211_ELEMID_SSID;
	j = strlen(ieee80211_assoc_ssid);
	*(write_ptr++) = j;
	for(i=0;i<j;i++)
		*(write_ptr++) = ieee80211_assoc_ssid[i];
	/* Supported Rates */
	*(write_ptr++) = IEEE80211_ELEMID_RATES;
	*(write_ptr++) = 1;
	*(write_ptr++) = 0x82;
	/* Current Channel */
	*(write_ptr++) = IEEE80211_ELEMID_DSPARMS;
	*(write_ptr++) = 1;
	*(write_ptr++) = ieee80211_assoc_channel;

	frame_length = sizeof(struct ieee80211_frame)+((unsigned int)write_ptr - (unsigned int)presp->presp);
	
	duration = rt2501_txtime(frame_length, 2)+IEEE80211_SIFS;
	presp->header.i_dur[0] = ((duration & 0x00ff) >> 0);
	presp->header.i_dur[1] = ((duration & 0xff00) >> 8);
	
	rt2501_make_tx_descriptor(
	&presp->txd,
	RT2501_CIPHER_NONE,		/* CipherAlg */
	0,				/* KeyTable */
	0,				/* KeyIdx */
	0,				/* Ack */
	0,				/* Fragment */
	1,				/* InsTimestamp */
	1,				/* RetryMode */
	0,				/* Ifs */
	RT2501_RATE_1,			/* Rate */
	frame_length,			/* Length */
	0,				/* QueIdx */
	0				/* PacketId */
	);

	if(!rt2501_tx(presp, sizeof(TXD_STRUC)+frame_length)) {
		DBG_WIFI("TX error in ieee80211_send_probe_response\r\n");
		return;
	}
}

static void ieee80211_input_mgt(char *frame, unsigned int length, short int rssi)
{
	struct ieee80211_frame *fr = (struct ieee80211_frame *)frame;
	char *frame_current, *frame_end;
	unsigned int i;

	if(length < sizeof(struct ieee80211_frame)) return;
	/* All management frames must be flagged "No DS" */
	if((fr->i_fc[1] & IEEE80211_FC1_DIR_MASK) != IEEE80211_FC1_DIR_NODS)
		return;
	if((ieee80211_mode == IEEE80211_M_MANAGED) && (ieee80211_state == IEEE80211_S_RUN))
		if(memcmp(fr->i_addr2, ieee80211_assoc_mac, IEEE80211_ADDR_LEN) == 0)
			ieee80211_new_rssi_sample(rssi);
	frame_current = frame+sizeof(struct ieee80211_frame);
	frame_end = frame+length;
	switch(fr->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) {
		case IEEE80211_FC0_SUBTYPE_PROBE_REQ:
			if(ieee80211_mode == IEEE80211_M_MASTER) {
				int ssid_present;
				char ssid[IEEE80211_SSID_MAXLEN+1];
				
				ssid_present = 0;
				while(frame_current < frame_end) {
					if(frame_current[0] == IEEE80211_ELEMID_SSID) {
						ssid_present = 1;
						for(i=0;i<frame_current[1];i++)
							ssid[i] = frame_current[i+2];
						ssid[i] = 0;
					}
					frame_current += (frame_current[1] + 2);
				}
				if(ssid[0] == 0) ssid_present = 0;
				
#ifdef DEBUG_WIFI
				if(!ssid_present) {
					DBG_WIFI("Received probe request with no SSID\r\n");
				} else {
					sprintf(dbg_buffer, "Received probe request with SSID \"%s\"\r\n", ssid);
					DBG_WIFI(dbg_buffer);
				}
#endif
				if((!ssid_present) || (strcmp(ssid, ieee80211_assoc_ssid) == 0))
					ieee80211_send_probe_response(fr->i_addr2);
			}
			break;
		case IEEE80211_FC0_SUBTYPE_PROBE_RESP:
			DBG_WIFI("[PR]\r\n");
			/* fall through */
		case IEEE80211_FC0_SUBTYPE_BEACON:
			/*
			We're only interested in beacons and probe responses
			- while scanning in Managed mode (1)
			- when associated, to check the AP is still alive (2)
			*/
//			DBG_WIFI("[BEACON]\r\n");
//                   dump(frame,length);
			if(ieee80211_mode == IEEE80211_M_MANAGED) {
				if(ieee80211_state == IEEE80211_S_SCAN) {
					/* (1) */
					struct rt2501_scan_result scan_result;
					unsigned short int capinfo;

					DBG_WIFI("Received beacon/PR while scanning\r\n");

					if((frame_end - frame_current) < 12) return;

					capinfo = (frame_current[10] << 0)|(frame_current[11] << 8);
					frame_current += 12; /* skip timestamp, beacon interval, capinfo */

					scan_result.ssid[0] = 0;
					memcpy(scan_result.mac, fr->i_addr2, IEEE80211_ADDR_LEN);
					memcpy(scan_result.bssid, fr->i_addr3, IEEE80211_ADDR_LEN);
					scan_result.channel = 0;
					scan_result.rssi = rssi;
					scan_result.rateset = 0;
					if(capinfo & IEEE80211_CAPINFO_PRIVACY)
						scan_result.encryption = IEEE80211_CRYPT_WEP64;
					else
						scan_result.encryption = IEEE80211_CRYPT_NONE;

					while(frame_current < frame_end) {
						switch(frame_current[0]) {
							case IEEE80211_ELEMID_SSID:
								if(frame_current[1] < sizeof(scan_result.ssid)) {
									for(i=0;i<frame_current[1];i++)
										scan_result.ssid[i] = frame_current[i+2];
									scan_result.ssid[i] = 0;
								}
								break;
							case IEEE80211_ELEMID_DSPARMS:
								scan_result.channel = frame_current[2];
								break;
							case IEEE80211_ELEMID_RATES:
							case IEEE80211_ELEMID_XRATES:
								for(i=0;i<frame_current[1];i++) {
									scan_result.rateset |= ieee80211_rate_to_mask(frame_current[i+2] & 0x7f);
#ifdef DEBUG_WIFI
									sprintf(dbg_buffer, "supported rate:0x%02x\r\n", frame_current[i+2]);
//									DBG_WIFI(dbg_buffer);
#endif
								}
								break;
							case IEEE80211_ELEMID_VENDOR: {
								/* Check for WPA */
								char *current;
								unsigned short int count;
								int found;
								
								/* Minimal size of a WPA element */
								if(frame_current[1] < 22) break;
								
								/* Check RSN IE */
								current = &frame_current[2];
								if(memcmp(current, ieee80211_vendor_wpa_id, sizeof(ieee80211_vendor_wpa_id)) != 0) break;
								current += sizeof(ieee80211_vendor_wpa_id);
								DBG_WIFI("WPA supported\r\n");
								
								/* Element 1: Multicast cipher suite (OUI) */
								if(memcmp(current, ieee80211_tkip_oui, IEEE80211_OUI_LEN) != 0) {
									scan_result.encryption = IEEE80211_CRYPT_WPA_UNSUPPORTED;
									break;
								}
								current += IEEE80211_OUI_LEN;
								
								/* Element 2: Number of unicast cipher suites, 2 bytes */
								count = (current[0] << 0)|(current[1] << 8);
								current += 2;
								
								/* Element 3: Unicast cipher suites (OUIs) */
								found = 0;
								for(i=0;i<count;i++) {
									if(memcmp(current, ieee80211_tkip_oui, IEEE80211_OUI_LEN) == 0) found = 1;
									current += IEEE80211_OUI_LEN;
								}
								if(!found) {
									scan_result.encryption = IEEE80211_CRYPT_WPA_UNSUPPORTED;
									break;
								}
								
								/* Element 4: Number of auth key management suites, 2 bytes */
								count = (current[0] << 0)|(current[1] << 8);
								current += 2;
								
								/* Element 5: Auth key management suites (OUIs) */
								found = 0;
								for(i=0;i<count;i++) {
									if(memcmp(current, ieee80211_psk_oui, IEEE80211_OUI_LEN) == 0) found = 1;
									current += IEEE80211_OUI_LEN;
								}
								if(!found) {
									scan_result.encryption = IEEE80211_CRYPT_WPA_UNSUPPORTED;
									break;
								}
								
								scan_result.encryption = IEEE80211_CRYPT_WPA;
								break;
							}
							default:
								break;
						}
						frame_current += (frame_current[1] + 2);
					}
					if(scan_result.rateset == 0) scan_result.rateset = IEEE80211_RATEMASK_1;

					ieee80211_scallback(&scan_result, ieee80211_scallback_userparam);
				}
				if(ieee80211_state == IEEE80211_S_RUN) {
					/* (2) */
					if((memcmp(fr->i_addr2, ieee80211_assoc_mac, IEEE80211_ADDR_LEN) == 0)
						&& (memcmp(fr->i_addr3, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN) == 0))
								ieee80211_timeout = IEEE80211_RUN_TIMEOUT;
				}
			} /* ieee80211_mode == IEEE80211_M_MANAGED */
			break;
		case IEEE80211_FC0_SUBTYPE_AUTH:
			if(length < (sizeof(struct ieee80211_frame)+6)) return;
			if(ieee80211_mode == IEEE80211_M_MANAGED) {
				/*
				Managed mode.
				If we are authenticating, check for possible AP reply.
				*/
				if(ieee80211_state == IEEE80211_S_AUTH) {
					if(memcmp(fr->i_addr1, rt2501_mac, IEEE80211_ADDR_LEN) != 0) break;
					if(memcmp(fr->i_addr2, ieee80211_assoc_mac, IEEE80211_ADDR_LEN) != 0) break;
					if(memcmp(fr->i_addr3, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN) != 0) break;
					if(ieee80211_authmode == IEEE80211_AUTH_OPEN) {
						/* Open authentication */
						if(((frame_current[0] << 0)|(frame_current[1] << 8)) != IEEE80211_AUTH_ALG_OPEN) {
							DBG_WIFI("Open auth failed: unexpected algo reply\r\n");
							ieee80211_state = IEEE80211_S_IDLE;
							break;
						}
						if(((frame_current[2] << 0)|(frame_current[3] << 8)) != IEEE80211_AUTH_OPEN_RESPONSE)  {
							DBG_WIFI("Open auth failed: unexpected sequence reply\r\n");
							ieee80211_state = IEEE80211_S_IDLE;
							break;
						}
						if(((frame_current[4] << 0)|(frame_current[5] << 8)) != IEEE80211_STATUS_SUCCESS) {
							DBG_WIFI("Open auth failed: denied by AP\r\n");
							ieee80211_state = IEEE80211_S_IDLE;
							break;
						}
						ieee80211_associate();
					} else {
						/* Shared key authentication */
						/* Algo, Auth SEQ, Status code, remaining of the frame */
						ieee80211_input_shared_auth((frame_current[0] << 0)|(frame_current[1] << 8),
								(frame_current[2] << 0)|(frame_current[3] << 8),
								(frame_current[4] << 0)|(frame_current[5] << 8),
								&frame_current[6], length-sizeof(struct ieee80211_frame)-6);
					}
				}
			} else {
				/*
				Master mode.
				Check for a station trying to authenticate to us.
				*/
				if(memcmp(fr->i_addr1, rt2501_mac, IEEE80211_ADDR_LEN) != 0) break;
				/* i_addr2 is the STA MAC */
				if(memcmp(fr->i_addr3, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN) != 0) break;
				/* STA MAC, Algorithm, Auth SEQ, Status code */
				ieee80211_auth_sta(fr->i_addr2,
						(frame_current[0] << 0)|(frame_current[1] << 8),
						(frame_current[2] << 0)|(frame_current[3] << 8),
						(frame_current[4] << 0)|(frame_current[5] << 8));
			}
			break;
		case IEEE80211_FC0_SUBTYPE_ASSOC_REQ:
			/*
			We're only insterested in them when in Master mode,
			to take care of authenticated stations trying to associate
			to us.
			*/
			if(ieee80211_mode == IEEE80211_M_MASTER) {
				int sta_slot;

				if(memcmp(fr->i_addr1, rt2501_mac, IEEE80211_ADDR_LEN) != 0) break;
				/* i_addr2 is the STA MAC */
				if(memcmp(fr->i_addr3, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN) != 0) break;
				sta_slot = ieee80211_find_sta_slot(fr->i_addr2);
				if(sta_slot != -1) {
					/*
					We have only one SSID, the station should not
					request another. Always accept association.
					*/
					ieee80211_send_assocresp(fr->i_addr2,
							IEEE80211_STATUS_SUCCESS,
							sta_slot+1);
					ieee80211_associated_sta[sta_slot].state = IEEE80211_S_RUN;
					ieee80211_associated_sta[sta_slot].timer = IEEE80211_STA_MAX_IDLE;
				} else {
					DBG_WIFI("Assoc request from a not authenticated station\r\n");
				}
			}
			break;
		case IEEE80211_FC0_SUBTYPE_ASSOC_RESP:
			/*
			Association Responses are only interesting after we have
			sent an Association Request, in Managed mode only.
			*/
			if((ieee80211_mode == IEEE80211_M_MANAGED) && (ieee80211_state == IEEE80211_S_ASSOC)) {
				unsigned int assoc_code;

				if(memcmp(fr->i_addr1, rt2501_mac, IEEE80211_ADDR_LEN) != 0) break;
				if(memcmp(fr->i_addr2, ieee80211_assoc_mac, IEEE80211_ADDR_LEN) != 0) break;
				if(memcmp(fr->i_addr3, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN) != 0) break;

				assoc_code = ((frame_current[2] << 0)|(frame_current[3] << 8));
				if(assoc_code != IEEE80211_STATUS_SUCCESS)  {
#ifdef DEBUG_WIFI
					sprintf(dbg_buffer, "Assoc failed by AP (0x%04x)\r\n", assoc_code);
					DBG_WIFI(dbg_buffer);
#endif
					ieee80211_state = IEEE80211_S_IDLE;
					break;
				}
#ifdef DEBUG_WIFI
				sprintf(dbg_buffer, "Association successful with \"%s\"\r\n", ieee80211_assoc_ssid);
				DBG_WIFI(dbg_buffer);
#endif
				ieee80211_rssi_sample_index = 0;
				ieee80211_rssi_average = -100;
				/* ieee80211_txrate was filled at auth */
				if(ieee80211_encryption == IEEE80211_CRYPT_WPA) {
					ieee80211_state = IEEE80211_S_EAPOL;
					ieee80211_timeout = IEEE80211_EAPOL_TIMEOUT;
				} else {
					ieee80211_state = IEEE80211_S_RUN;
					ieee80211_timeout = IEEE80211_RUN_TIMEOUT;
				}
			}
			break;
		case IEEE80211_FC0_SUBTYPE_DISASSOC:
		case IEEE80211_FC0_SUBTYPE_DEAUTH:
			if(ieee80211_mode == IEEE80211_M_MANAGED) {
				/* Managed mode */
				if(ieee80211_state != IEEE80211_S_IDLE) {
					if(memcmp(fr->i_addr1, rt2501_mac, IEEE80211_ADDR_LEN) != 0) break;
					if(memcmp(fr->i_addr2, ieee80211_assoc_mac, IEEE80211_ADDR_LEN) != 0) break;
					if(memcmp(fr->i_addr3, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN) != 0) break;
					
#ifdef DEBUG_WIFI
					sprintf(dbg_buffer, "Lost connection (0x%04x) !\r\n",
						(frame_current[0] << 0)|(frame_current[1] << 8));
					DBG_WIFI(dbg_buffer);
#endif
					ieee80211_state = IEEE80211_S_IDLE;
				}
			} else {
				/* Master mode */
				int sta_slot;

				if(memcmp(fr->i_addr1, rt2501_mac, IEEE80211_ADDR_LEN) != 0) break;
				if(memcmp(fr->i_addr3, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN) != 0) break;
				sta_slot = ieee80211_find_sta_slot(fr->i_addr2);
				if(sta_slot == -1) break;
				if((fr->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) == IEEE80211_FC0_SUBTYPE_DISASSOC) {
					ieee80211_associated_sta[sta_slot].state = IEEE80211_S_AUTH;
					ieee80211_associated_sta[sta_slot].timer = IEEE80211_STA_ASSOC_TIMEOUT;
				} else ieee80211_associated_sta[sta_slot].state = IEEE80211_S_IDLE;
			}
			break;
		default:
			break;
	}
}

static void ieee80211_input_ctl(char *frame, unsigned int length)
{
	DBG_WIFI("Received control frame\r\n");
	/* handled by the RT2501 ASIC */
}

static void ieee80211_input_data(char *frame, unsigned int length, short int rssi)
{
#pragma pack(1)
	struct {
	struct ieee80211_frame header;
	char data[];
	} *fr;
#pragma pack()
	unsigned char *source_mac, *dest_mac;

/*        DBG_WIFI("ieee80211_input_data\r\n"); */

	if((ieee80211_state != IEEE80211_S_EAPOL)
	   && (ieee80211_state != IEEE80211_S_RUN)) return;
	if(length < sizeof(struct ieee80211_frame)) return;
	fr = (void *)frame;

	/* In Managed mode, we are only interested in frames From DS */
	if(((fr->header.i_fc[1] & IEEE80211_FC1_DIR_MASK) == IEEE80211_FC1_DIR_FROMDS)
		    && (ieee80211_mode == IEEE80211_M_MANAGED)) {
		/* Destination address */
		if((memcmp(fr->header.i_addr1, rt2501_mac, IEEE80211_ADDR_LEN) != 0)
				  && (memcmp(fr->header.i_addr1, ieee80211_broadcast_address, IEEE80211_ADDR_LEN) != 0)) return;
		dest_mac = fr->header.i_addr1;
		/* BSSID */
		if(memcmp(fr->header.i_addr2, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN) != 0) return;
		/* Source address (i_addr3) can be anything */
		source_mac = fr->header.i_addr3;
		ieee80211_new_rssi_sample(rssi);
	} else {
		/* In Master mode, we are only interested in frames To DS */
		if(((fr->header.i_fc[1] & IEEE80211_FC1_DIR_MASK) == IEEE80211_FC1_DIR_TODS)
			&& (ieee80211_mode == IEEE80211_M_MASTER)) {
			int sta_slot;

			/* BSSID */
			if(memcmp(fr->header.i_addr1, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN) != 0) return;

			/*
			Source address:
			Check the station is associated, and reset its timer
			*/
			sta_slot = ieee80211_find_sta_slot(fr->header.i_addr2);

			if((sta_slot == -1) || (ieee80211_associated_sta[sta_slot].state != IEEE80211_S_RUN)) {
				DBG_WIFI("Data frame from not associated station, dropped\r\n");
				return;
			}
			ieee80211_associated_sta[sta_slot].timer = IEEE80211_STA_MAX_IDLE;
			source_mac = fr->header.i_addr2;

			/*
			Destination address must be the device's MAC or
			broadcast, as we don't route frames between
			associated stations in this driver.
			*/
			if((memcmp(fr->header.i_addr3, rt2501_mac, IEEE80211_ADDR_LEN) != 0)
				&& (memcmp(fr->header.i_addr3, ieee80211_broadcast_address, IEEE80211_ADDR_LEN) != 0)) return;
			dest_mac = fr->header.i_addr3;
		} else return; /* Drop other frames */
	}

	switch(fr->header.i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) {
	case IEEE80211_FC0_SUBTYPE_DATA:
		if(!rt2501buffer_new(fr->data,
			length-sizeof(struct ieee80211_frame),
			source_mac, dest_mac))
				DBG_WIFI("Unable to queue up received data frame\r\n");
		break;
	case IEEE80211_FC0_SUBTYPE_NODATA:
		if((ieee80211_mode == IEEE80211_M_MANAGED)
			&& (memcmp(source_mac, ieee80211_assoc_mac, IEEE80211_ADDR_LEN) == 0)) {
			DBG_WIFI("Ping from AP, replying\r\n");
			rt2501_send(NULL, 0, ieee80211_assoc_mac, 1, 0);
		}
		break;
	default:
	    break;
	}
}

void ieee80211_input(char *frame, unsigned int length, short int rssi)
{
	struct ieee80211_frame_min *frame_min;

	if(ieee80211_state == IEEE80211_S_IDLE) return;

	if(length < sizeof(struct ieee80211_frame_min)) return;
	frame_min = (struct ieee80211_frame_min *)frame;

	switch(frame_min->i_fc[0] & IEEE80211_FC0_TYPE_MASK) {
		case IEEE80211_FC0_TYPE_MGT:
			ieee80211_input_mgt(frame, length, rssi);
			break;
		case IEEE80211_FC0_TYPE_CTL:
			ieee80211_input_ctl(frame, length);
			break;
		case IEEE80211_FC0_TYPE_DATA:
			ieee80211_input_data(frame, length, rssi);
			break;
	}
}

void ieee80211_init(void)
{
	rt2501_setmode(IEEE80211_M_MANAGED, NULL, 0);
}

static void ieee80211_start_beacon()
{
#pragma pack(1)
	struct {
		TXD_STRUC txd;
		struct ieee80211_frame header;
		char beacon[4 				/* Fixed Parameters */
			+2+IEEE80211_SSID_MAXLEN 	/* SSID */
			+2+8				/* Rates */
			+2+4				/* Extended rates */
			+2+1				/* Channel */
			+3];				/* Possible alignment */
	} beacon;
#pragma pack()
	char *write_ptr;
	unsigned int i, j;
	unsigned int frame_length;

	DBG_WIFI("Starting beacon emission\r\n");

	beacon.header.i_fc[0] = IEEE80211_FC0_TYPE_MGT|IEEE80211_FC0_SUBTYPE_BEACON
			|(IEEE80211_FC0_VERSION_0 << IEEE80211_FC0_VERSION_SHIFT);
	beacon.header.i_fc[1] = IEEE80211_FC1_DIR_NODS;
	beacon.header.i_dur[0] = 0;
	beacon.header.i_dur[1] = 0;
	memcpy(beacon.header.i_addr1, ieee80211_broadcast_address, IEEE80211_ADDR_LEN);
	memcpy(beacon.header.i_addr2, rt2501_mac, IEEE80211_ADDR_LEN);
	memcpy(beacon.header.i_addr3, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN);
	beacon.header.i_seq[0] = 0;
	beacon.header.i_seq[1] = 0;

	write_ptr = beacon.beacon;
	/* FIXED PARAMETERS */
	/* Timestamp, handled by Ralink chip */
	*(write_ptr++) = 0x00;
	*(write_ptr++) = 0x00;
	*(write_ptr++) = 0x00;
	*(write_ptr++) = 0x00;
	*(write_ptr++) = 0x00;
	*(write_ptr++) = 0x00;
	*(write_ptr++) = 0x00;
	*(write_ptr++) = 0x00;
	/* Beacon Interval */
	*(write_ptr++) = 0x64;
	*(write_ptr++) = 0x00;
	/* Capability Information */
	*(write_ptr++) = ((IEEE80211_CAPINFO_ESS & 0x00ff) >> 0);
	*(write_ptr++) = ((IEEE80211_CAPINFO_ESS & 0xff00) >> 8);
	/* TAGGED PARAMETERS */
	/* SSID */
	*(write_ptr++) = IEEE80211_ELEMID_SSID;
	j = strlen(ieee80211_assoc_ssid);
	*(write_ptr++) = j;
	for(i=0;i<j;i++)
		*(write_ptr++) = ieee80211_assoc_ssid[i];
	/* Supported Rates */
	*(write_ptr++) = IEEE80211_ELEMID_RATES;
	*(write_ptr++) = 1;
	*(write_ptr++) = 0x82;
	/* Current Channel */
	*(write_ptr++) = IEEE80211_ELEMID_DSPARMS;
	*(write_ptr++) = 1;
	*(write_ptr++) = ieee80211_assoc_channel;

	frame_length = sizeof(struct ieee80211_frame)+((unsigned int)write_ptr - (unsigned int)beacon.beacon);
	rt2501_make_tx_descriptor(
	&beacon.txd,
	RT2501_CIPHER_NONE,		/* CipherAlg */
	0,				/* KeyTable */
	0,				/* KeyIdx */
	0,				/* Ack */
	0,				/* Fragment */
	1,				/* InsTimestamp */
	1,				/* RetryMode */
	0,				/* Ifs */
	RT2501_RATE_1,			/* Rate */
	frame_length,			/* Length */
	0,				/* QueIdx */
	0				/* PacketId */
	);

	rt2501_beacon(&beacon, frame_length+sizeof(TXD_STRUC));
}

static void ieee80211_stop_beacon(void)
{
	rt2501_beacon(NULL, 0);
}

void rt2501_setmode(int mode, const char *ssid, unsigned char channel)
{
	int i;
	struct rt2501buffer *b;

	switch(mode) {
		case IEEE80211_M_MANAGED:
			DBG_WIFI("Switching to Managed mode\r\n");

			disable_ohci_irq();
			ieee80211_state = IEEE80211_S_IDLE;
			ieee80211_mode = IEEE80211_M_MANAGED;
			enable_ohci_irq();

			ieee80211_stop_beacon();
			break;
		case IEEE80211_M_MASTER:
			DBG_WIFI("Switching to Master mode\r\n");

			disable_ohci_irq();
			ieee80211_state = IEEE80211_S_RUN;
			ieee80211_mode = IEEE80211_M_MASTER;
			strcpy(ieee80211_assoc_ssid, ssid);
			memcpy(ieee80211_assoc_bssid, rt2501_mac, IEEE80211_ADDR_LEN);
			ieee80211_assoc_channel = channel;
			for(i=0;i<RT2501_MAX_ASSOCIATED_STA;i++)
				ieee80211_associated_sta[i].state = IEEE80211_S_IDLE;
			ieee80211_encryption = IEEE80211_CRYPT_NONE;
			ieee80211_assoc_rateset = ieee80211_txrate = ieee80211_lowest_txrate = IEEE80211_RATEMASK_1;
			enable_ohci_irq();

			/* Update ASIC BSSID */
			rt2501_set_bssid(ieee80211_assoc_bssid);

			/* AP only supports 1Mbps. Tell the ASIC autoresponder. */
			rt2501_write(rt2501_dev, RT2501_TXRX_CSR5, RT2501_RATEMASK_1);

			rt2501_switch_channel(ieee80211_assoc_channel);

			ieee80211_start_beacon();

			break;
	}
	/* Remove all RX queued data */
	disable_ohci_irq();
	do {
		b = rt2501_receive();
		hcd_free(b);
	} while(b != NULL);
	enable_ohci_irq();
}

void rt2501_scan(const char *ssid, rt2501_scan_callback callback, void *userparam)
{
#pragma pack(1)
	struct {
	TXD_STRUC txd;
	struct ieee80211_frame header;
	char probe[2+IEEE80211_SSID_MAXLEN+2+8+2+4];
	} *probe;
#pragma pack()
	char *write_ptr;
	unsigned int frame_length;
	unsigned char channel, i, j;

	if(ieee80211_mode != IEEE80211_M_MANAGED) return;

	DBG_WIFI("Scanning...\r\n");

	/* Set up the state machine */
	ieee80211_scallback = callback;
	ieee80211_scallback_userparam = userparam;
	ieee80211_state = IEEE80211_S_SCAN;

	/* Set no BSSID */
	rt2501_set_bssid(ieee80211_null_address);

	for(channel=1;channel<RT2501_MAX_NUM_OF_CHANNELS+1;channel++) {
#ifdef DEBUG_WIFI
		sprintf(dbg_buffer, "channel %d\r\n", channel);
		DBG_WIFI(dbg_buffer);
#endif
		rt2501_switch_channel(channel);

		disable_ohci_irq();
		probe = hcd_malloc(sizeof(*probe)+7, COMRAM,14);
		enable_ohci_irq();
		if(probe == NULL) {
			DBG_WIFI("hcd_malloc failed in rt2501_scan\r\n");
			return;
		}

		/* Send the probe request */
		probe->header.i_fc[0] = IEEE80211_FC0_TYPE_MGT|IEEE80211_FC0_SUBTYPE_PROBE_REQ
				|(IEEE80211_FC0_VERSION_0 << IEEE80211_FC0_VERSION_SHIFT);
		probe->header.i_fc[1] = IEEE80211_FC1_DIR_NODS;
		probe->header.i_dur[0] = 0;
		probe->header.i_dur[1] = 0;
		memcpy(probe->header.i_addr1, ieee80211_broadcast_address, IEEE80211_ADDR_LEN);
		memcpy(probe->header.i_addr2, rt2501_mac, IEEE80211_ADDR_LEN);
		memcpy(probe->header.i_addr3, ieee80211_broadcast_address, IEEE80211_ADDR_LEN);
		probe->header.i_seq[0] = 0;
		probe->header.i_seq[1] = 0;

		write_ptr = probe->probe;
		/* TAGGED PARAMETERS */
		if(ssid != NULL) {
			/* SSID */
			*(write_ptr++) = IEEE80211_ELEMID_SSID;
			j = strlen(ssid);
			*(write_ptr++) = j;
			for(i=0;i<j;i++)
				*(write_ptr++) = ssid[i];
		}
		/* Supported Rates */
		*(write_ptr++) = IEEE80211_ELEMID_RATES;
		*(write_ptr++) = 8;
		*(write_ptr++) = 0x02;
		*(write_ptr++) = 0x04;
		*(write_ptr++) = 0x0b;
		*(write_ptr++) = 0x16;
		*(write_ptr++) = 0x24;
		*(write_ptr++) = 0x30;
		*(write_ptr++) = 0x48;
		*(write_ptr++) = 0x6c;
		/* Extended Supported Rates */
		*(write_ptr++) = IEEE80211_ELEMID_XRATES;
		*(write_ptr++) = 4;
		*(write_ptr++) = 0x0c;
		*(write_ptr++) = 0x12;
		*(write_ptr++) = 0x18;
		*(write_ptr++) = 0x60;

		frame_length = sizeof(struct ieee80211_frame)+((unsigned int)write_ptr - (unsigned int)probe->probe);

		rt2501_make_tx_descriptor(
		&probe->txd,
		RT2501_CIPHER_NONE,		/* CipherAlg */
		0,				/* KeyTable */
		0,				/* KeyIdx */
		1,				/* Ack */
		0,				/* Fragment */
		0,				/* InsTimestamp */
		1,				/* RetryMode */
		0,				/* Ifs */
		RT2501_RATE_1,			/* Rate */
		frame_length,			/* Length */
		0,				/* QueIdx */
		0				/* PacketId */
					 );

		if(!rt2501_tx(probe, frame_length+sizeof(TXD_STRUC)))
			DBG_WIFI("Unable to send probe request !\r\n");
		DelayMs(350);
	}
	DBG_WIFI("\r\n");

	ieee80211_state = IEEE80211_S_IDLE;
}

void rt2501_auth(const char *ssid, const unsigned char *mac,
		 const unsigned char *bssid, unsigned char channel,
		 unsigned short int rateset,
		 unsigned char authmode,
		 unsigned char encryption,
		 const unsigned char *key)
{
#pragma pack(1)
	struct {
		TXD_STRUC txd;
		struct ieee80211_frame header;
		char auth[6];
	} *auth;
#pragma pack()
	unsigned short int duration;

	if(ieee80211_mode != IEEE80211_M_MANAGED) return;

//#ifdef DEBUG_WIFI
	sprintf(dbg_buffer, "Connecting to \"%s\" (%02x:%02x:%02x:%02x:%02x:%02x) on channel %d\r\n",
		ssid, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], channel);
	DBG_WIFI(dbg_buffer);
//#endif

	/* Store network parameters */
	ieee80211_state = IEEE80211_S_IDLE;

	memcpy(ieee80211_assoc_mac, mac, IEEE80211_ADDR_LEN);
	memcpy(ieee80211_assoc_bssid, bssid, IEEE80211_ADDR_LEN);
	strcpy(ieee80211_assoc_ssid, ssid);

	ieee80211_assoc_channel = channel;

	ieee80211_assoc_rateset = rateset;
// add cc
        gb_encryption=0;
	sprintf(dbg_buffer, "encr:0x%02x" , encryption);
	DBG_WIFI(dbg_buffer);
        if (encryption==5) { encryption=3; gb_encryption=1;}
//
	ieee80211_encryption = encryption;
	switch(ieee80211_encryption) {
		case IEEE80211_CRYPT_NONE:
			ieee80211_authmode = IEEE80211_AUTH_OPEN;
			rt2501_set_key(0, NULL, NULL, NULL, RT2501_CIPHER_NONE);
			break;
		case IEEE80211_CRYPT_WEP64:
			ieee80211_authmode = authmode;
			memcpy(ieee80211_key, key, IEEE80211_WEP64_KEYLEN);
			rt2501_set_key(0, ieee80211_key, NULL, NULL, RT2501_CIPHER_WEP64);
			break;
		case IEEE80211_CRYPT_WEP128:
			ieee80211_authmode = authmode;
			memcpy(ieee80211_key, key, IEEE80211_WEP128_KEYLEN);
			rt2501_set_key(0, ieee80211_key, NULL, NULL, RT2501_CIPHER_WEP128);
			break;
		case IEEE80211_CRYPT_WPA:
			ieee80211_authmode = IEEE80211_AUTH_OPEN;
			strcpy((char *)ieee80211_key, (const char *)key);
			rt2501_set_key(0, NULL, NULL, NULL, RT2501_CIPHER_NONE);
			eapol_init();
			break;
		default:
			DBG_WIFI("Unknown encryption specified\r\n");
			return;
	}

	ieee80211_lowest_txrate = ieee80211_find_closest_rate(IEEE80211_RATEMASK_1);
	ieee80211_txrate = ieee80211_lowest_txrate;
#ifdef DEBUG_WIFI
	sprintf(dbg_buffer, "supported rates 0x%08x, lowest TX rate 0x%08x\r\n",
		ieee80211_assoc_rateset,
		ieee80211_lowest_txrate);
	DBG_WIFI(dbg_buffer);
#endif

	rt2501_set_bssid(ieee80211_assoc_bssid);

	DBG_WIFI("Sending auth request\r\n");

	/* Send the initial AUTH frame */
	do {
		disable_ohci_irq();
		auth = hcd_malloc(sizeof(*auth)+7, COMRAM,15);
		enable_ohci_irq();
	} while(auth == NULL);

	/*
	We dont use ieee80211_send_basic_auth here because we want to support
	other algorithms.
	*/
	auth->header.i_fc[0] = IEEE80211_FC0_TYPE_MGT|IEEE80211_FC0_SUBTYPE_AUTH
			|(IEEE80211_FC0_VERSION_0 << IEEE80211_FC0_VERSION_SHIFT);
	auth->header.i_fc[1] = IEEE80211_FC1_DIR_NODS;
	duration = rt2501_txtime(sizeof(*auth)-sizeof(TXD_STRUC), ieee80211_mask_to_rate(ieee80211_lowest_txrate))+IEEE80211_SIFS;
	auth->header.i_dur[0] = ((duration & 0x00ff) >> 0);
	auth->header.i_dur[1] = ((duration & 0xff00) >> 8);
	memcpy(auth->header.i_addr1, mac, IEEE80211_ADDR_LEN);
	memcpy(auth->header.i_addr2, rt2501_mac, IEEE80211_ADDR_LEN);
	memcpy(auth->header.i_addr3, bssid, IEEE80211_ADDR_LEN);
	auth->header.i_seq[0] = 0;
	auth->header.i_seq[1] = 0;

	if(authmode == IEEE80211_AUTH_OPEN) {
		/* Open authentication: algo[2], seq[2], status[2] */
		auth->auth[0] = ((IEEE80211_AUTH_ALG_OPEN & 0x00ff) >> 0);
		auth->auth[1] = ((IEEE80211_AUTH_ALG_OPEN & 0xff00) >> 8);
		auth->auth[2] = ((IEEE80211_AUTH_OPEN_REQUEST & 0x00ff) >> 0);
		auth->auth[3] = ((IEEE80211_AUTH_OPEN_REQUEST & 0xff00) >> 8);
		auth->auth[4] = ((IEEE80211_STATUS_SUCCESS & 0x00ff) >> 0);
		auth->auth[5] = ((IEEE80211_STATUS_SUCCESS & 0xff00) >> 8);
	} else {
		/* Shared key authentication: algo[2], seq[2], status[2] */
		auth->auth[0] = ((IEEE80211_AUTH_ALG_SHARED & 0x00ff) >> 0);
		auth->auth[1] = ((IEEE80211_AUTH_ALG_SHARED & 0xff00) >> 8);
		auth->auth[2] = ((IEEE80211_AUTH_SHARED_REQUEST & 0x00ff) >> 0);
		auth->auth[3] = ((IEEE80211_AUTH_SHARED_REQUEST & 0xff00) >> 8);
		auth->auth[4] = ((IEEE80211_STATUS_SUCCESS & 0x00ff) >> 0);
		auth->auth[5] = ((IEEE80211_STATUS_SUCCESS & 0xff00) >> 8);
	}

	/* Tell the ASIC autoresponder the rates the AP supports */
	rt2501_write(rt2501_dev, RT2501_TXRX_CSR5,
		     ieee80211_mask_to_rt2501mask(ieee80211_assoc_rateset));

	rt2501_switch_channel(ieee80211_assoc_channel);

	rt2501_make_tx_descriptor(
	&auth->txd,
	RT2501_CIPHER_NONE,			/* CipherAlg */
	0,					/* KeyTable */
	0,					/* KeyIdx */
	1,					/* Ack */
	0,					/* Fragment */
	0,					/* InsTimestamp */
	1,					/* RetryMode */
	0,					/* Ifs */
	/* Rate */
	ieee80211_mask_to_rt2501rate(ieee80211_lowest_txrate),
	sizeof(*auth)-sizeof(TXD_STRUC),	/* Length */
	0,					/* QueIdx */
	0					/* PacketId */
				 );

	disable_ohci_irq();
	if(!rt2501_tx(auth, sizeof(*auth))) {
		DBG_WIFI("TX error in rt2501_auth\r\n");
		enable_ohci_irq();
		return;
	}
	/* Frame sent, update the state machine */
	ieee80211_state = IEEE80211_S_AUTH;
	ieee80211_timeout = IEEE80211_AUTH_TIMEOUT;
	enable_ohci_irq();
}

int rt2501_send(const unsigned char *frame, unsigned int length, const unsigned char *dest_mac,
		int lowrate, int mayblock)
{
#pragma pack(1)
	struct {
		TXD_STRUC txd;
		struct ieee80211_frame header;
		unsigned char data[];
	} *fr;
#pragma pack()
	unsigned char encryption;
	unsigned char encryption_overhead;
	unsigned short int duration;

	if((ieee80211_state != IEEE80211_S_EAPOL)
	   && (ieee80211_state != IEEE80211_S_RUN)) return 0;
	if(frame == NULL) length = 0;

	do {
		disable_ohci_irq();
		fr = hcd_malloc(sizeof(TXD_STRUC)+sizeof(struct ieee80211_frame)+length+7,
				COMRAM,16);
		enable_ohci_irq();
	} while((fr == NULL) && mayblock);
	if(fr == NULL) return 0;

	fr->header.i_fc[0] = IEEE80211_FC0_TYPE_DATA|(IEEE80211_FC0_VERSION_0 << IEEE80211_FC0_VERSION_SHIFT);
	if(length == 0) {
		fr->header.i_fc[0] |= IEEE80211_FC0_SUBTYPE_NODATA;
		encryption = IEEE80211_CRYPT_NONE;
	} else {
		fr->header.i_fc[0] |= IEEE80211_FC0_SUBTYPE_DATA;
		if(ieee80211_encryption == IEEE80211_CRYPT_WPA) {
			if((eapol_state == EAPOL_S_MSG1) || (eapol_state == EAPOL_S_MSG3))
				encryption = IEEE80211_CRYPT_NONE;
			else
				encryption = IEEE80211_CRYPT_WPA;
		} else encryption = ieee80211_encryption;
	}
	
	if(ieee80211_mode == IEEE80211_M_MANAGED)
		fr->header.i_fc[1] = IEEE80211_FC1_DIR_TODS;
	else
		fr->header.i_fc[1] = IEEE80211_FC1_DIR_FROMDS;

	if(encryption != IEEE80211_CRYPT_NONE)
		fr->header.i_fc[1] |= IEEE80211_FC1_PROTECTED;

	switch(encryption) {
		case IEEE80211_CRYPT_NONE:
			encryption_overhead = 0;
			break;
		case IEEE80211_CRYPT_WEP64:
		case IEEE80211_CRYPT_WEP128:
			encryption_overhead = 8; /* IV[4] + ICV [4] */
			break;
		case IEEE80211_CRYPT_WPA:
			encryption_overhead = 20; /* (TKIP) IV[4] + EIV[4] + ICV[4] + MIC[8] */
			break;
	}

	if((ieee80211_mode == IEEE80211_M_MASTER) && (IEEE80211_IS_MULTICAST(dest_mac)))
		duration = 0;
	else
		duration = rt2501_txtime(sizeof(struct ieee80211_frame)+length+encryption_overhead, ieee80211_mask_to_rate(lowrate ? ieee80211_lowest_txrate : ieee80211_txrate))+IEEE80211_SIFS;
	fr->header.i_dur[0] = (duration & 0x00ff) >> 0;
	fr->header.i_dur[1] = (duration & 0xff00) >> 8;

	if(ieee80211_mode == IEEE80211_M_MANAGED) {
		memcpy(fr->header.i_addr1, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN);
		memcpy(fr->header.i_addr2, rt2501_mac, IEEE80211_ADDR_LEN);
		memcpy(fr->header.i_addr3, dest_mac, IEEE80211_ADDR_LEN);
	} else {
		memcpy(fr->header.i_addr1, dest_mac, IEEE80211_ADDR_LEN);
		memcpy(fr->header.i_addr2, ieee80211_assoc_bssid, IEEE80211_ADDR_LEN);
		memcpy(fr->header.i_addr3, rt2501_mac, IEEE80211_ADDR_LEN);
	}

	/* Sequence number is filled in by the ASIC */
	fr->header.i_seq[0] = 0;
	fr->header.i_seq[1] = 0;

	if(length > 0) memcpy(fr->data, frame, length);

	rt2501_make_tx_descriptor(
	&fr->txd,
	/* CipherAlg */
	ieee80211_crypt_to_rt2501cipher(encryption),
	0,					/* KeyTable */
	0,					/* KeyIdx */
	/* Ack */
	(ieee80211_mode == IEEE80211_M_MANAGED) /* AP should ack all our frames */
			|| !(IEEE80211_IS_MULTICAST(dest_mac)), /* When in AP mode, only unicast frames should be acked */
	0,					/* Fragment */
	0,					/* InsTimestamp */
	1,					/* RetryMode */
	0,					/* Ifs */
	/* Rate */
	ieee80211_mask_to_rt2501rate(lowrate ? ieee80211_lowest_txrate : ieee80211_txrate),
	sizeof(struct ieee80211_frame)+length,	/* Length */
	0,					/* QueIdx */
	0					/* PacketId */
	);
	if((encryption == IEEE80211_CRYPT_WEP64) || (encryption == IEEE80211_CRYPT_WEP128)) {
		fr->txd.Iv = rand() & 0x00ffffff;
		fr->txd.IvOffset = sizeof(struct ieee80211_frame);
	}
	if(encryption == IEEE80211_CRYPT_WPA) {
		struct ieee80211_tkip_iv iv;
		unsigned int i;

		iv.iv16.field.rc0 = ptk_tsc[1];
		iv.iv16.field.rc1 = (iv.iv16.field.rc0 | 0x20) & 0x7f;
		iv.iv16.field.rc2 = ptk_tsc[0];
		iv.iv16.field.control.field.reserved = 0;
		iv.iv16.field.control.field.ext_iv = 1;
		iv.iv16.field.control.field.key_id = 0;
//		iv.iv32 = *((unsigned int *)&ptk_tsc[2]);
                iv.iv32 = ptk_tsc[2] | (ptk_tsc[3] << 8) | (ptk_tsc[4] << 16) | (ptk_tsc[5] << 24);

		
		fr->txd.Iv = iv.iv16.word;
		fr->txd.Eiv = iv.iv32;
		fr->txd.IvOffset = sizeof(struct ieee80211_frame);
		
		i = 0;
		while(++ptk_tsc[i] == 0) {													\
			i++;
			if(i == EAPOL_TSC_LENGTH) {
				DBG_WIFI("TSC cycle !!!\r\n");
				break;
			}
					
		}
	}
	
#ifdef DEBUG_WIFI
	sprintf(dbg_buffer, "in rt2501_send, encryption=%d, length=%d, fc0=0x%02x, fc1=0x%02x\r\n",
		encryption, length, fr->header.i_fc[0], fr->header.i_fc[1]);
	DBG_WIFI(dbg_buffer);
#endif
	
	disable_ohci_irq();
	if(!rt2501_tx(fr, sizeof(TXD_STRUC)+sizeof(struct ieee80211_frame)+length)) {
		DBG_WIFI("TX error in rt2501_send\r\n");
		enable_ohci_irq();
		return 0;
	}
	enable_ohci_irq();
	DBG_WIFI("TX done in rt2501_send\r\n");
	return 1;
}

void ieee80211_timer(void)
{
	disable_ohci_irq();
	if(ieee80211_mode == IEEE80211_M_MANAGED) {
		/* Managed mode */
		if(ieee80211_timeout > 0) ieee80211_timeout--;
		if(ieee80211_timeout == 0)
			ieee80211_state = IEEE80211_S_IDLE;
	} else {
		/* Master mode */
		unsigned int i;

		for(i=0;i<RT2501_MAX_ASSOCIATED_STA;i++) {
			if(ieee80211_associated_sta[i].state != IEEE80211_S_IDLE) {
				if(ieee80211_associated_sta[i].timer > 0)
					ieee80211_associated_sta[i].timer--;
				if(ieee80211_associated_sta[i].timer == 0)
					ieee80211_deauth_sta(i, IEEE80211_REASON_AUTH_EXPIRE);
			}
		}
	}
	enable_ohci_irq();
}

int rt2501_rssi_average(void)
{
  return ieee80211_rssi_average;
}
