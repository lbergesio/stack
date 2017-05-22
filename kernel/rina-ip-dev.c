/*
 * RINA IP virtual network device (rina_ip_dev)
 *
 *    Leonardo Bergesio     <leonardo.bergesio@i2cat.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
/* For net_device related code */

#include <linux/if_arp.h>
#include <linux/ip.h>

#define RINA_PREFIX "rina-ip-dev"

#include "logs.h"
#include "debug.h"
#include "kfa.h"
#include "sdu.h"
#include "rds/rmem.h"
#include "rina-ip-dev.h"


struct rina_ip_dev {
	struct net_device_stats stats;
	struct ipcp_instance* kfa_ipcp;
	port_id_t port;
	struct net_device* dev;
};

static int rina_ip_dev_open(struct net_device *dev)
{
	LOG_DBG("RINA IP device %s opened...", dev->name);
	return 0;
}

static int rina_ip_dev_close(struct net_device *dev)
{
	LOG_DBG("RINA IP device %s closed...", dev->name);
	return 0;
}

static struct net_device_stats *rina_ip_dev_get_stats(struct net_device *dev)
{
    struct rina_ip_dev* ip_dev = netdev_priv(dev);
    if(!ip_dev)
	    return NULL;
    return &ip_dev->stats;
}

int rina_ip_dev_rcv(struct sk_buff *skb, struct rina_ip_dev *ip_dev)
{
	int rv;
	struct packet_type *ptype, *pt_prev = NULL;

	list_for_each_entry_rcu(ptype, &skb->dev->ptype_all, list) {
		if (ptype->type != skb->protocol)
			continue;
		if (pt_prev) {
			if (unlikely(skb_orphan_frags(skb, GFP_ATOMIC)))
				return -1;
			atomic_inc(&skb->users);

			LOG_DBG("RINA IP device %s rcv a packet...",
							ip_dev->dev->name);

			//return pt_prev->func(skb, skb->dev, pt_prev, NULL);
			rv = pt_prev->func(skb, ip_dev->dev, pt_prev, NULL);
			if(rv){
				ip_dev->stats.rx_dropped++;
			} else {
				ip_dev->stats.rx_packets++;
				ip_dev->stats.rx_bytes += skb->len;
			}
			return rv;
		}
		pt_prev = ptype;
	}

	ip_dev->stats.rx_dropped++;
	kfree_skb(skb);
	LOG_ERR("Could not find IP handler for packet %p and RINA IP device %s",
							skb, ip_dev->dev->name);

	return -1;
}

static int rina_ip_dev_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct iphdr* iph = NULL;
	struct sdu* sdu;
	struct rina_ip_dev* ip_dev = netdev_priv(dev);
	ASSERT(ip_dev);

	skb_orphan(skb);
	//skb_dst_force(skb);

	iph = ip_hdr(skb);
	ASSERT(iph);

	sdu = sdu_create_from_skb(skb);
	if (!sdu){
		kfree_skb(skb);
		ip_dev->stats.tx_dropped++;
		return NET_XMIT_DROP;
	}

        if(kfa_flow_sdu_write(ip_dev->kfa_ipcp->data, ip_dev->port, sdu,
									false)){
		ip_dev->stats.tx_dropped++;
		LOG_ERR("Could not xmit IP packet, unable to send to KFA...");
		return NET_XMIT_DROP;
	}

	ip_dev->stats.tx_packets++;
	ip_dev->stats.tx_bytes += skb->len;
	LOG_DBG("RINA IP device %s sent a packet...", dev->name);
	return NETDEV_TX_OK;
}

static const struct net_device_ops rina_ip_dev_ops = {
	.ndo_start_xmit	= rina_ip_dev_start_xmit,
	.ndo_get_stats	= rina_ip_dev_get_stats,
	.ndo_open	= rina_ip_dev_open,
	.ndo_stop	= rina_ip_dev_close,
};

/* as ether_setup to set internal dev fields */
static void rina_ip_dev_setup(struct net_device *dev)
{
	/*Mix of properties from lo and tun ifaces */
	/* This should be set depending on supporting DIF */
	dev->mtu = 64 * 1024;
	dev->hard_header_len = 0;
	dev->addr_len = 0;
	dev->type = ARPHRD_NONE;
	dev->flags = IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST;
	dev->priv_flags	|= IFF_LIVE_ADDR_CHANGE | IFF_NO_QUEUE
		| IFF_DONT_BRIDGE
		| IFF_PHONY_HEADROOM;
	netif_keep_dst(dev);
	dev->hw_features = NETIF_F_GSO_SOFTWARE;
	dev->features = NETIF_F_SG | NETIF_F_FRAGLIST
		| NETIF_F_GSO_SOFTWARE
		| NETIF_F_HW_CSUM
		| NETIF_F_RXCSUM
		| NETIF_F_SCTP_CRC
		| NETIF_F_HIGHDMA
		| NETIF_F_LLTX
		/* This should be removed when netns are supported */
		| NETIF_F_NETNS_LOCAL
		| NETIF_F_VLAN_CHALLENGED
		| NETIF_F_LOOPBACK;
	/*
	dev->ethtool_ops	= &loopback_ethtool_ops;
	dev->header_ops		= &eth_header_ops;
	dev->destructor		= loopback_dev_free;
	*/
	dev->netdev_ops	= &rina_ip_dev_ops;
	//dev->tx_queue_len = 0;

	return;
}

struct rina_ip_dev* rina_ip_dev_create(string_t* name,
					struct ipcp_instance* kfa_ipcp,
					port_id_t port)
{
	int rv;
	struct net_device *dev;
	struct rina_ip_dev* ip_dev;

	if (!kfa_ipcp || !name)
		return NULL;

	dev = alloc_netdev(sizeof(struct rina_ip_dev), name,
							NET_NAME_UNKNOWN,
			      				rina_ip_dev_setup);
	if (!dev) {
		LOG_ERR("Could not allocate RINA IP network device %s", name);
		return NULL;
	}

	//SET_NETDEV_DEV(dev, parent);

	ip_dev = netdev_priv(dev);
	ip_dev->dev = dev;
	ip_dev->kfa_ipcp = kfa_ipcp;
	ip_dev->port = port;
	memset(&ip_dev->stats, 0x00, sizeof(struct net_device_stats));

	rv = register_netdev(dev);
	if(rv) {
		LOG_ERR("Could not register RINA IP device %s: %d", name, rv);
		free_netdev(dev);
		return NULL;
	}

	LOG_DBG("RINA IP device %s (%pk) created with dev %p", name, ip_dev,
									dev);

	return ip_dev;
}

int rina_ip_dev_destroy(struct rina_ip_dev *ip_dev)
{
	if(!ip_dev)
		return -1;

	unregister_netdev(ip_dev->dev);
	free_netdev(ip_dev->dev);
	return 0;
}
