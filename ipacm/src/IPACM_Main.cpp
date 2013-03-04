/* 
Copyright (c) 2013, The Linux Foundation. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
		* Redistributions of source code must retain the above copyright
			notice, this list of conditions and the following disclaimer.
		* Redistributions in binary form must reproduce the above
			copyright notice, this list of conditions and the following
			disclaimer in the documentation and/or other materials provided
			with the distribution.
		* Neither the name of The Linux Foundation nor the names of its
			contributors may be used to endorse or promote products derived
			from this software without specific prior written permission.																								from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/*!
	@file
	IPACM_Main.cpp

	@brief
	This file implements the IPAM functionality.

	@Author
	Skylar Chang

*/
/******************************************************************************

																																																																																																IP_MAIN.C

******************************************************************************/

#include <sys/socket.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <stdlib.h>

#include "IPACM_CmdQueue.h"
#include "IPACM_EvtDispatcher.h"
#include "IPACM_Defs.h"
#include "IPACM_Neighbor.h"
#include "IPACM_IfaceManager.h"
#include "IPACM_Log.h"

#include "IPACM_ConntrackListener.h"

#define IPA_DRIVER  "/dev/ipa"

#define IPACM_DIR_NAME     "/etc"
#define IPACM_FILE_NAME    "mobileap_firewall.xml"

#define INOTIFY_EVENT_SIZE  (sizeof(struct inotify_event))
#define INOTIFY_BUF_LEN     (INOTIFY_EVENT_SIZE + 2*sizeof(IPACM_FILE_NAME))

#define IPA_DRIVER_WLAN_EVENT_SIZE  (sizeof(struct ipa_wlan_msg))
#define IPA_DRIVER_WLAN_META_MSG    (sizeof(struct ipa_msg_meta))
#define IPA_DRIVER_WLAN_BUF_LEN     (IPA_DRIVER_WLAN_EVENT_SIZE + IPA_DRIVER_WLAN_META_MSG)

//char *log_buf = (char *)malloc(LOG_SIZE);
int ipa_get_if_index(char *if_name, int *if_index);

/* start netlink socket monitor*/
void* netlink_start(void *param)
{
	ipa_nl_sk_fd_set_info_t sk_fdset;
	int ret_val = 0;

	memset(&sk_fdset, 0, sizeof(ipa_nl_sk_fd_set_info_t));
	IPACMDBG("netlink starter memset sk_fdset succeeds\n");
	ret_val = ipa_nl_listener_init(NETLINK_ROUTE, (RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE | RTMGRP_LINK |
																										RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | RTMGRP_NEIGH |
																										RTNLGRP_IPV6_PREFIX),
																 &sk_fdset, ipa_nl_recv_msg);

	if (ret_val != IPACM_SUCCESS)
	{
		IPACMERR("Failed to initialize IPA netlink event listener\n");
		return NULL;
	}

	return NULL;
}

/* start firewall-rule monitor*/
void* firewall_monitor(void *param)
{
	int length;
	int wd;
	char buffer[INOTIFY_BUF_LEN];
	int inotify_fd;
	ipacm_cmd_q_data evt_data;
	uint32_t mask = IN_MODIFY;

	inotify_fd = inotify_init();
	if (inotify_fd < 0)
	{
		PERROR("inotify_init");
	}

	IPACMDBG("Waiting for nofications in dir %s with mask: 0x%x\n", IPACM_DIR_NAME, mask);

	wd = inotify_add_watch(inotify_fd,
												 IPACM_DIR_NAME,
												 mask);

	while (1)
	{
		length = read(inotify_fd, buffer, INOTIFY_BUF_LEN);
		struct inotify_event *event = (struct inotify_event *)buffer;

		if (length < 0)
		{
			IPACMDBG("inotify read() error return length: %d and mask: 0x%x 0x%x\n", length, event->mask, mask);
			return NULL;
		}

		if (event->len > 0)
		{
			if (event->mask & IN_MODIFY)
			{
				if (event->mask & IN_ISDIR)
				{
					IPACMDBG("The directory %s was 0x%x\n", event->name, event->mask);
				}
				else if (!strncmp(event->name, IPACM_FILE_NAME, event->len))
				{
					IPACMDBG("File \"%s\" was 0x%x\n", event->name, event->mask);
					IPACMDBG("The interested file %s .\n", IPACM_FILE_NAME);

					evt_data.event = IPA_FIREWALL_CHANGE_EVENT;
					evt_data.evt_data = NULL;

					/* Insert IPA_FIREWALL_CHANGE_EVENT to command queue */
					IPACM_EvtDispatcher::PostEvt(&evt_data);
				}
			}
			IPACMDBG("Received monitoring event %s.\n", event->name);
		}
	}

	(void)inotify_rm_watch(inotify_fd, wd);
	(void)close(inotify_fd);
	return NULL;
}


/* start IPACM WLAN-driver notifier */
void* ipa_driver_wlan_notifier(void *param)
{
	int length, fd;
	char buffer[IPA_DRIVER_WLAN_BUF_LEN];
	struct ipa_msg_meta *event_hdr = NULL;
	struct ipa_wlan_msg *event = NULL;

	ipacm_cmd_q_data evt_data;
	ipacm_event_data_mac *data = NULL;

	fd = open(IPA_DRIVER, O_RDWR);
	if (fd == 0)
	{
		IPACMERR("Failed opening %s.\n", IPA_DRIVER);
	}
	
	while (1)
	{
		IPACMDBG("Waiting for nofications from IPA driver \n");
		memset(buffer, 0, sizeof(buffer));
		memset(&evt_data, 0, sizeof(evt_data));
		data = NULL;

		length = read(fd, buffer, IPA_DRIVER_WLAN_BUF_LEN);
		if (length < 0)
		{
			PERROR("didn't read IPA_driver correctly");
			return NULL;
		}

		event_hdr = (struct ipa_msg_meta *)buffer;
		IPACMDBG("Message type: %d\n", event_hdr->msg_type);
		IPACMDBG("Event header length received: %d\n",event_hdr->msg_len);

		if (event_hdr->msg_len > 0)
		{
			event = (struct ipa_wlan_msg *)(buffer + sizeof(struct ipa_msg_meta));
		}

		/* Insert WLAN_DRIVER_EVENT to command queue */
		data = (ipacm_event_data_mac *)malloc(sizeof(ipacm_event_data_mac));
		if (data == NULL)
		{
			PERROR("unable to allocate memory for event data\n");
			return NULL;
		}
		
		switch (event_hdr->msg_type)
		{

		case SW_ROUTING_ENABLE:
			IPACMDBG("Received SW_ROUTING_ENABLE\n");
			evt_data.event = IPA_SW_ROUTING_ENABLE;
			evt_data.evt_data = NULL;
			break;

		case SW_ROUTING_DISABLE:
			IPACMDBG("Received SW_ROUTING_DISABLE\n");
			evt_data.event = IPA_SW_ROUTING_DISABLE;
			evt_data.evt_data = NULL;
			break;

		case WLAN_CLIENT_CONNECT:
			IPACMDBG("Received WLAN_CLIENT_CONNECT\n");
			IPACMDBG("Mac Address %02x:%02x:%02x:%02x:%02x:%02x\n",
							 event->mac_addr[0], event->mac_addr[1], event->mac_addr[2],
							 event->mac_addr[3], event->mac_addr[4], event->mac_addr[5]); 

		  evt_data.event = IPA_WLAN_CLIENT_ADD_EVENT;
			evt_data.evt_data = data;
			ipa_get_if_index(event->name, &(data->if_index));
			memcpy(data->mac_addr,
						 event->mac_addr,
						 sizeof(event->mac_addr));
			break;
			
		case WLAN_CLIENT_DISCONNECT:
			IPACMDBG("Received WLAN_CLIENT_DISCONNECT\n");
			IPACMDBG("Mac Address %02x:%02x:%02x:%02x:%02x:%02x\n",
							 event->mac_addr[0], event->mac_addr[1], event->mac_addr[2],
							 event->mac_addr[3], event->mac_addr[4], event->mac_addr[5]); 

			memcpy(data->mac_addr,
						 event->mac_addr,
						 sizeof(event->mac_addr));
			ipa_get_if_index(event->name, &(data->if_index));

			evt_data.event = IPA_WLAN_CLIENT_DEL_EVENT;
			evt_data.evt_data = data;
			
			break;

		case WLAN_CLIENT_POWER_SAVE_MODE:
			IPACMDBG("Received WLAN_CLIENT_POWER_SAVE_MODE\n");
			IPACMDBG("Mac Address [0]:%2d [1]:%2d [2]:%2d [3]:%2d [4]:%2d [5]%2d\n",
							 event->mac_addr[0], event->mac_addr[1], event->mac_addr[2],
							 event->mac_addr[3], event->mac_addr[4], event->mac_addr[5]); 

			ipa_get_if_index(event->name, &(data->if_index));

			evt_data.event = IPA_WLAN_CLIENT_POWER_SAVE_EVENT;
			evt_data.evt_data = data;
			memcpy(data->mac_addr,
						 event->mac_addr,
						 sizeof(event->mac_addr));
			
			break;

		case WLAN_CLIENT_NORMAL_MODE:
			IPACMDBG("Received WLAN_CLIENT_NORMAL_MODE\n");
			IPACMDBG("Mac Address [0]:%2d [1]:%2d [2]:%2d [3]:%2d [4]:%2d [5]%2d\n",
							 event->mac_addr[0], event->mac_addr[1], event->mac_addr[2],
							 event->mac_addr[3], event->mac_addr[4], event->mac_addr[5]); 

			memcpy(data->mac_addr,
						 event->mac_addr,
						 sizeof(event->mac_addr));
			ipa_get_if_index(event->name, &(data->if_index));
			evt_data.evt_data = data;
			evt_data.event = IPA_WLAN_CLIENT_RECOVER_EVENT;
			break;

		default:
			IPACMDBG("Invalid message\n");
			free(data);
			continue;

		}

		/* finish command queue */
		if (evt_data.evt_data == NULL)
		{
			free(data);
		}

		IPACMDBG("Posting event:%d\n", evt_data.event);
		IPACM_EvtDispatcher::PostEvt(&evt_data);
	}

	(void)close(fd);
	return NULL;
}


int main(int argc, char **argv)
{
	int ret;
	pthread_t netlink_thread = 0, monitor_thread = 0, ipa_driver_thread = 0;
	pthread_t cmd_queue_thread = 0;

	IPACM_Neighbor *neigh = new IPACM_Neighbor();
	IPACM_IfaceManager *ifacemgr = new IPACM_IfaceManager();

	IPACMDBG("Staring IPA main\n");
	IPACMDBG("ipa_cmdq_successful\n");


	if (IPACM_SUCCESS == cmd_queue_thread)
	{
		ret = pthread_create(&cmd_queue_thread, NULL, MessageQueue::Process, NULL);
		if (IPACM_SUCCESS != ret)
		{
			IPACMERR("unable to command queue thread\n");
			return ret;
		}
		IPACMDBG("created command queue thread\n");
	}

	if (IPACM_SUCCESS == netlink_thread)
	{
		ret = pthread_create(&netlink_thread, NULL, netlink_start, NULL);
		if (IPACM_SUCCESS != ret)
		{
			IPACMERR("unable to create netlink thread\n");
			return ret;
		}
		IPACMDBG("created netlink thread\n");
	}


	if (IPACM_SUCCESS == monitor_thread)
	{
		ret = pthread_create(&monitor_thread, NULL, firewall_monitor, NULL);
		if (IPACM_SUCCESS != ret)
		{
			IPACMERR("unable to create monitor thread\n");
			return ret;
		}
		IPACMDBG("created firewall monitor thread\n");
	}

	if (IPACM_SUCCESS == ipa_driver_thread)
	{
		ret = pthread_create(&ipa_driver_thread, NULL, ipa_driver_wlan_notifier, NULL);
		if (IPACM_SUCCESS != ret)
		{
			IPACMERR("unable to create ipa_driver_wlan thread\n");
			return ret;
		}
		IPACMDBG("created ipa_driver_wlan thread\n");
	}

	pthread_join(cmd_queue_thread, NULL);
	pthread_join(netlink_thread, NULL);
	pthread_join(monitor_thread, NULL);
	pthread_join(ipa_driver_thread, NULL);
	return IPACM_SUCCESS;
}


/*===========================================================================
		FUNCTION  ipa_get_if_index
===========================================================================*/
/*!
@brief
  get ipa interface index by given the interface name

@return
	IPACM_SUCCESS or IPA_FALUIRE

@note

- Dependencies
		- None

- Side Effects
		- None
*/
/*=========================================================================*/
int ipa_get_if_index
(
	 char *if_name,
	 int *if_index
	 )
{
	int fd;
	struct ifreq ifr;

	if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
	{
		PERROR("get interface index socket create failed");
		return IPACM_FAILURE;
	}

	memset(&ifr, 0, sizeof(struct ifreq));

	(void)strncpy(ifr.ifr_name, if_name, sizeof(ifr.ifr_name));

	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
	{
		PERROR("call_ioctl_on_dev: ioctl failed:");
		close(fd);
		return IPACM_FAILURE;
	}

	*if_index = ifr.ifr_ifindex;
	close(fd);
	return IPACM_SUCCESS;
}