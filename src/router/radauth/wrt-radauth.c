/* Really simple radius authenticator
 *
 * Copyright (c) 2004 Michael Gernoth <michael@gernoth.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include "radius.h"

#define WAIT	300	/* Seconds until a STA expires */


struct  sta {
	        unsigned char mac[6];
		unsigned char accepted;
		unsigned char changed;
		struct sta *next; /* Pointer to next STA in linked list */
		time_t lastseen;
};

char *server, *secret;
short port;
short mackey;
short macfmt;
int internal=0;
#include <shutils.h>

/* Broadcom */
#ifndef HAVE_MADWIFI
#include <wlutils.h>
#include <wlioctl.h>

#include <bcmnvram.h>
static void set_maclist(char *iface,char *buf)
{
	wl_ioctl(iface, WLC_SET_MACLIST, buf, WLC_IOCTL_MAXLEN);
}
static void security_disable(char *iface)
{
	int val;
	val = WLC_MACMODE_DISABLED;
	wl_ioctl(iface,WLC_SET_MACMODE, &val, sizeof(val));
}
static void security_deny(char *iface)
{
	int val;
	val = WLC_MACMODE_DENY;
	wl_ioctl(iface,WLC_SET_MACMODE, &val, sizeof(val));
}
static void kick_mac(char *iface,char *mac)
{
	wl_ioctl(iface, 0x8f, mac, 6);	/* Kick station off AP */
}
#else
#include <sys/types.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/ethernet.h>
#include <bcmnvram.h>

#include "wireless.h"
#include "net80211/ieee80211.h"
#include "net80211/ieee80211_crypto.h"
#include "net80211/ieee80211_ioctl.h"
#define WLC_IOCTL_MAXLEN	8192


/* Atheros */

static int socket_handle = -1;


static int
getsocket (void)
{

  if (socket_handle < 0)
    {
      socket_handle = socket (AF_INET, SOCK_DGRAM, 0);
      if (socket_handle < 0)
	err (1, "socket(SOCK_DGRAM)");
    }
  return socket_handle;
}

#define IOCTL_ERR(x) [x - SIOCIWFIRSTPRIV] "ioctl[" #x "]"
static int
set80211priv (struct iwreq *iwr, const char *ifname, int op, void *data,
	     size_t len)
{
#define	N(a)	(sizeof(a)/sizeof(a[0]))

  memset (iwr, 0, sizeof (struct iwreq));
  strncpy (iwr->ifr_name, ifname, IFNAMSIZ);
  if (len < IFNAMSIZ)
    {
      /*
       * Argument data fits inline; put it there.
       */
      memcpy (iwr->u.name, data, len);
    }
  else
    {
      /*
       * Argument data too big for inline transfer; setup a
       * parameter block instead; the kernel will transfer
       * the data for the driver.
       */
      iwr->u.data.pointer = data;
      iwr->u.data.length = len;
    }

  if (ioctl (getsocket (), op, iwr) < 0)
    {
      static const char *opnames[] = {
	IOCTL_ERR (IEEE80211_IOCTL_SETPARAM),
	IOCTL_ERR (IEEE80211_IOCTL_GETPARAM),
	IOCTL_ERR (IEEE80211_IOCTL_SETMODE),
	IOCTL_ERR (IEEE80211_IOCTL_GETMODE),
	IOCTL_ERR (IEEE80211_IOCTL_SETWMMPARAMS),
	IOCTL_ERR (IEEE80211_IOCTL_GETWMMPARAMS),
	IOCTL_ERR (IEEE80211_IOCTL_SETCHANLIST),
	IOCTL_ERR (IEEE80211_IOCTL_GETCHANLIST),
	IOCTL_ERR (IEEE80211_IOCTL_CHANSWITCH),
	IOCTL_ERR (IEEE80211_IOCTL_GETCHANINFO),
	IOCTL_ERR (IEEE80211_IOCTL_SETOPTIE),
	IOCTL_ERR (IEEE80211_IOCTL_GETOPTIE),
	IOCTL_ERR (IEEE80211_IOCTL_SETMLME),
	IOCTL_ERR (IEEE80211_IOCTL_SETKEY),
	IOCTL_ERR (IEEE80211_IOCTL_DELKEY),
	IOCTL_ERR (IEEE80211_IOCTL_ADDMAC),
	IOCTL_ERR (IEEE80211_IOCTL_DELMAC),
	IOCTL_ERR (IEEE80211_IOCTL_WDSADDMAC),
	IOCTL_ERR (IEEE80211_IOCTL_WDSDELMAC),
      };
      op -= SIOCIWFIRSTPRIV;
      if (0 <= op && op < N (opnames))
	perror (opnames[op]);
      else
	perror ("ioctl[unknown???]");
      return -1;
    }
  return 0;
#undef N
}

static int
do80211priv (const char *ifname, int op, void *data, size_t len)
{
  struct iwreq iwr;

  if (set80211priv (&iwr, ifname, op, data, len) < 0)
    return -1;
  if (len < IFNAMSIZ)
    memcpy (data, iwr.u.name, len);
  return iwr.u.data.length;
}

static int
set80211param(char *iface, int op, int arg)
{
	struct iwreq iwr;

	memset(&iwr, 0, sizeof(iwr));
	strncpy(iwr.ifr_name, iface, IFNAMSIZ);
	iwr.u.mode = op;
	memcpy(iwr.u.name+sizeof(__u32), &arg, sizeof(arg));

	if (ioctl(getsocket(), IEEE80211_IOCTL_SETPARAM, &iwr) < 0) {
		perror("ioctl[IEEE80211_IOCTL_SETPARAM]");
		return -1;
	}
	return 0;
}



struct maclist {
	uint count;			/* number of MAC addresses */
	struct ether_addr ea[1];	/* variable length array of MAC addresses */
};

static void security_disable(char *iface)
{
#ifdef DEBUG
	printf("Security Disable\n");
#endif
    set80211param(iface,IEEE80211_PARAM_MACCMD,IEEE80211_MACCMD_FLUSH);
    set80211param(iface,IEEE80211_PARAM_MACCMD,IEEE80211_MACCMD_POLICY_OPEN);
    
}
static void set_maclist(char *iface,char *buf)
{
	struct ieee80211req_mlme mlme;
	struct maclist *maclist = (struct maclist *) buf;

	if (maclist->count==0)
	    security_disable();
//	if (authorized)
//		mlme.im_op = IEEE80211_MLME_AUTHORIZE;
//	else
	uint i;
	for (i=0;i<maclist->count;i++)
	    {
		mlme.im_op = IEEE80211_MLME_UNAUTHORIZE;
		mlme.im_reason = 0;
		memcpy(mlme.im_macaddr, &maclist->ea[i], IEEE80211_ADDR_LEN);
		do80211priv(iface, IEEE80211_IOCTL_SETMLME, &mlme,sizeof(mlme));
	    }
}
static void security_deny(char *iface)
{
#ifdef DEBUG
	printf("Policy Deny\n");
#endif
    set80211param(iface,IEEE80211_PARAM_MACCMD,IEEE80211_MACCMD_POLICY_DENY);
}
static void kick_mac(char *iface,char *mac)
{
#ifdef DEBUG
	printf("KickMac: %s\n",mac);
#endif
	struct ieee80211req_mlme mlme;
	mlme.im_op = IEEE80211_MLME_DISASSOC;
	//mlme.im_reason = IEEE80211_REASON_UNSPECIFIED;
	mlme.im_reason = IEEE80211_REASON_NOT_AUTHED;
        memcpy(mlme.im_macaddr,mac,6);
	
	
	do80211priv(iface, IEEE80211_IOCTL_SETMLME, &mlme, sizeof(mlme));
}
#endif

#define tou(a) if (a>('a'-1) && a<('z'+1) ){ a-='a'; a+='A';}
int stricmp(unsigned char *c1,unsigned char *c2)
{
if (c1==NULL && c2==NULL)return 0;
if (c1==NULL)return -1;
if (c2==NULL)return -1;
int i;
for (i=0;i<strlen(c1);i++)
    tou(c1[i]);

for (i=0;i<strlen(c2);i++)
    tou(c2[i]);
return strcmp(c1,c2);
}

int authmac(unsigned char *mac)
{
char macstr[32];
if (internal)
    {
	sprintf(macstr,"%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x",\
		mac[0],mac[1],mac[2],mac[3],\
		mac[4],mac[5]);
#ifdef DEBUG
	printf("mac %s\n",macstr);
#endif
    

    char *collection=nvram_get_collection("iradius");
    if (collection!=NULL)
    {
    char entry[32];
    char *next;
    int c=0;
    foreach(entry,collection,next)
	{
	if (!(c%2))
	if (!stricmp(entry,macstr))
	    {
	    if (next!=NULL)
		{
#ifdef DEBUG
	printf("next %s\n",next);
#endif
		if (strncmp(next," 1",2)==0)
		    {
		    free(collection);
		    return 1;
		    }
		    else
		    {
		    free(collection);
		    return 0;
		    }
		}
	    }
	c++;
	}
    }
    if (collection==NULL)
    {
    collection=malloc(32);
    memset(collection,0,32);
    }
    else
    collection=realloc(collection,strlen(collection)+32);
#ifdef DEBUG
	printf("mac2 %s\n",macstr);
#endif
    strcat(collection,macstr);
    strcat(collection," 0 ");
#ifdef DEBUG
	printf("collection %s\n",collection);
#endif
    nvram_store_collection("iradius",collection);

int radcount=0;
char *radc=nvram_get("iradius_count");
if (radc!=NULL)radcount=atoi(radc);

radcount++;
char count[16];
sprintf(count,"%d",radcount);
nvram_set("iradius_count",count);
nvram_commit();
    free(collection);
    return 0;
    }else{


	switch(macfmt)
	{
	case 1: //000000-000000
	sprintf(macstr,"%2.2x%2.2x%2.2x-%2.2x%2.2x%2.2x",\
		mac[0],mac[1],mac[2],mac[3],\
		mac[4],mac[5]);
	break;  
	case 2: //000000000000
	sprintf(macstr,"%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x",\
		mac[0],mac[1],mac[2],mac[3],\
		mac[4],mac[5]);
	break;
	case 3: //00:00:00:00:00:00
	sprintf(macstr,"%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x",\
		mac[0],mac[1],mac[2],mac[3],\
		mac[4],mac[5]);
	break;
	default: //00-00-00-00-00-00
	sprintf(macstr,"%2.2X-%2.2X-%2.2X-%2.2X-%2.2X-%2.2X",\
			mac[0],mac[1],mac[2],mac[3],\
			mac[4],mac[5]);
	}
	if (!mackey)
	return radius(server, port, macstr, macstr, secret);
	else
	return radius(server, port, macstr, secret, secret);
}
	
	
}

int main(int argc, char** argv)
{
	int num,i;
	unsigned char buf[WLC_IOCTL_MAXLEN];	/* Buffer for wireless-ioctls MAC lists */
	unsigned char *pos;
	char *iface;
	char *maxun;
	int override;
	struct maclist *maclist;
	struct ether_addr *ea;
	int usePortal=0;
	char macbuild[64];
	struct sta *first;	/* Pointer to first element in linked STA list */
	
	int val;
	int lastcnt;		/* Number of blacklisted cards in the last loop */
	int statechange;	/* Do we need to push the new blacklist/reset the card? */
	time_t step;
	/* SeG DD-WRT change */
	int unauthenticated_users; /* count for unauthicated users which can access the AP without radius */
	int maxunauthenticated_users; /* maxcount for unauthenticated users */


	if (argc < 2 )
	{
	argerror:;
		fprintf(stderr,"wrt-radauth - A simple radius authenticator\n");
		fprintf(stderr,"(C) 2005 Michael Gernoth\n");
		fprintf(stderr,"(C) 2006 Atheros support Sebastian Gottschall\n");
		fprintf(stderr,"Usage: %s [-nx] interface radiusip radiusport sharedkey radiusoverride mackeytype macunauthusers\n",argv[0]);
		fprintf(stderr,"\t-n1\tUse new MAC address format 'aabbcc-ddeeff' instead of 'AA-BB-CC-DD-EE-FF'\n");
		fprintf(stderr,"\t-n2\tUse really new MAC address format 'aabbccddeeff' instead of 'AA-BB-CC-DD-EE-FF'\n");	
		exit(1);
	}

#ifdef DEBUG
	printf("$Id: wrt-radauth.c,v 1.17 2004/09/28 13:15:51 simigern Exp $ coming up...\n");
#endif
int offset=1;
	if (argc>2 && (strcmp(argv[1],"-n1") == 0))
	{
		macfmt=1;
		offset=2;
	}else
	if (argc>2 && (strcmp(argv[1],"-n2") == 0))
	{
		macfmt=2;
		offset=2;
	}else
	if (argc>2 && (strcmp(argv[1],"-n3") == 0))
	{
		macfmt=3;
		offset=2;
	}else
	if (argc>2 && (strcmp(argv[1],"-t") == 0))
	{
		macfmt=0;
		internal=1;
		offset=2;
	}
	else {
		macfmt=0;
		offset=1;
	}
	iface=argv[offset++];
	if (argc-offset!=6)
	    {
	     goto argerror;
	    }
	if (!internal)
	    {
	    usePortal=0;
	    server=argv[offset++];
	    port=atoi(argv[offset++]);
	    secret=argv[offset++];
	    override=atoi(argv[offset++]);	
	    mackey=atoi(argv[offset++]);	        
	    maxun=atoi(argv[offset++]);
	    }
#ifndef HAVE_MADWIFI
	if (wl_probe(iface))
	{
		printf("Interface %s is not broadcom wireless!\n",iface);
	}
#endif

	/* Get configuration from nvram */
/*	server=strdup(nvram_safe_get("wl0_radius_ipaddr"));
	port=atoi(nvram_safe_get("wl0_radius_port"));
	usePortal=atoi(nvram_safe_get("wl_radportal"));
        override=atoi(nvram_safe_get("radius_override"));
*/	
	/* SeG DD-WRT change */
//	maxun = nvram_get("max_unauth_users");
	if (maxun!=NULL && strlen(maxun)>0)
	    maxunauthenticated_users = atoi(maxun); //read nvram variable
	else
	    maxunauthenticated_users = 0;
//	secret=strdup(nvram_get("wl0_radius_key"));
//	mackey=atoi(nvram_get("wl_radmacpassword"));
#ifdef DEBUG
	printf("Server: %s:%d, Secret: %s\n",server,port,secret);
#endif

	/* Initialize vars */
	lastcnt=0;

	/* Disable MAC security on card */
	memset(buf,0,WLC_IOCTL_MAXLEN);	
	set_maclist(iface,buf);
	security_disable(iface);


	/* No STAs in list */
	first = NULL;

	while(1)
	{
		struct sta *currsta, *prev;

		step=time(NULL);

		/* Initialize vars */
		statechange=0;

		/* Query card for currently associated STAs */
		memset(buf,0,sizeof(buf));
#ifdef DEBUG
	puts("get assoc list");
#endif
		getassoclist(iface,buf);
#ifdef DEBUG
	puts("done()");
#endif
		pos=buf;
		memcpy(&num,pos,4);	/* TODO: This really is struct maclist */
		pos+=4;
#ifdef DEBUG
	printf("count %d\n",num);
#endif

		unauthenticated_users = 0; //reset count for unauthenticated users
		
		/* Look at the associated STAs */
		for(i=0; i<num; i++)
		{
			currsta = first;
			prev = NULL;

			/* Have we already seen this STA */
			while ( currsta != NULL )
			{
				if (memcmp(currsta->mac,pos,6) == 0)
				{
					if ( currsta->lastseen+WAIT < step )
					{
						currsta->changed=1;
						int response = authmac(pos);
						if (response==-10 && override==1)
						    currsta->accepted=1;
						else
						if (response>0)
						{
							currsta->accepted=1;
#ifdef DEBUG
							printf("Reauthenticating STA %2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x\n",
									currsta->mac[0], currsta->mac[1],
									currsta->mac[2], currsta->mac[3],
									currsta->mac[4], currsta->mac[5]);
#endif
						} else {
							currsta->accepted=0;
						}
						currsta->lastseen = step;
					}

					if ( ! currsta->accepted )
					{
						/* SeG DD-WRT Change  */
						if (unauthenticated_users<maxunauthenticated_users)
						{
						  unauthenticated_users++; //increment unauthenticated user count
						  currsta->accepted = 1;
						}else
						{
						  currsta->changed=1;
#ifdef DEBUG
						  printf("Rejecting STA %2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x\n",
								currsta->mac[0], currsta->mac[1],
								currsta->mac[2], currsta->mac[3],
								currsta->mac[4], currsta->mac[5]);
#endif						
						}

					}
					break;
				}
				prev = currsta;
				currsta = currsta->next;
			}
			
			/* Or is it new? */
			if ( currsta == NULL )
			{
				/* Alloc mem for new STA */
				currsta = malloc(sizeof(struct sta));
				if ( currsta == NULL )
				{
					perror("malloc");
					exit(1);
				}

				if ( first == NULL )
				{
					first = currsta;
				} else {
					prev->next = currsta;
				}

				currsta->next = NULL;
				currsta->changed = 1;

				memcpy(currsta->mac,pos,6);
				currsta->lastseen = step;
				int response = authmac(currsta->mac);
				if (response==-10 && override==1)
				    currsta->accepted = 1;
				else
				if (response==1)
				{
					currsta->accepted = 1;
#ifdef DEBUG
					printf("Accepting STA %2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x\n",
							currsta->mac[0], currsta->mac[1],
							currsta->mac[2], currsta->mac[3],
							currsta->mac[4], currsta->mac[5]);
#endif
					if (usePortal)
					{
					sprintf(macbuild,"radiusallow %2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x\n",
							currsta->mac[0], currsta->mac[1],
							currsta->mac[2], currsta->mac[3],
							currsta->mac[4], currsta->mac[5]);
					system(macbuild);
				
					}

				} else {
				        /* SeG DD-WRT Change  */
					if (unauthenticated_users<maxunauthenticated_users)
					{
					unauthenticated_users++; //increment unauthenticated user count
					currsta->accepted = 1;
					}else
					{
					currsta->accepted = 0;
#ifdef DEBUG
					printf("Rejecting STA %2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x\n",
							currsta->mac[0], currsta->mac[1],
							currsta->mac[2], currsta->mac[3],
							currsta->mac[4], currsta->mac[5]);
#endif
					}
				}
			}
			pos+=6;	/* Jump to next MAC in list */
		}

		/* Expire old STAs from list, free memory */
		currsta = first;
		prev = NULL;
		while ( currsta != NULL )
		{
			if ( currsta->lastseen+WAIT < step )
			{
				struct sta *tmpsta;

				statechange=1;
#ifdef DEBUG
				printf("Expiring STA %2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x\n",
						currsta->mac[0], currsta->mac[1],
						currsta->mac[2], currsta->mac[3],
						currsta->mac[4], currsta->mac[5]);
#endif
				
				if ( currsta == first )
				{
					tmpsta = first;
					first = first->next;
					free(tmpsta);
					currsta = first;
				} else {
					tmpsta = currsta;
					prev->next = currsta->next;
					free(currsta);
					currsta = prev->next;
				}

			} else {
				prev = currsta;
				currsta = currsta->next;
			}
		}

		/* Find STAs to kick off */
		memset(buf,0,sizeof(buf));
		maclist = (struct maclist *) buf;
		maclist->count=0;
		ea = maclist->ea;
		currsta = first;
		while ( currsta != NULL )
		{
			if ( ! currsta->accepted )
			{
				memcpy(ea,currsta->mac,6);
				ea++;
				maclist->count++;
			}
			if ( currsta->changed )
			{
				statechange=1;
				currsta->changed=0;
			}
			currsta = currsta->next;
		}

		/* statechange = Previously unseen/denied STA seen or STA expired */
		if ( statechange )
		{
			if ( maclist->count )
			{
				unsigned char mac[6];
				
				
				set_maclist(iface,buf);  //set maclist to deny
				security_deny(iface); //deny macs

				ea = maclist->ea;
				for(i=0; i < maclist->count; i++)
				{
					memcpy(mac,ea,6);
#ifdef DEBUG
					printf("Disassociating STA %2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x\n",
							mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#endif					
					if (usePortal)
					{
					sprintf(macbuild,"radiusdisallow %2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x\n",
							mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
					system(macbuild);
					
					}else
					kick_mac(iface,mac);
					
					
					ea++;
				}
			} else {
				if ( lastcnt != 0 )
				{
					/* The card does not accept any association with an empty
					 * deny-list, so disable MAC-security */
#ifdef DEBUG
					printf("Resetting security\n");
#endif
					security_disable(iface);
				}
			}
			lastcnt = maclist->count;
		}

		/* Immediately continue after a statechange */
		if ( ! statechange )
			sleep(1);

	}
	
	exit(0);
}
