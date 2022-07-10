/*
 * Utility to find IPP printers via Bonjour/DNS-SD and optionally run
 * commands such as IPP and Bonjour conformance tests.  This tool is
 * inspired by the UNIX "find" command, thus its name.
 *
 * Copyright © 2021-2022 by OpenPrinting.
 * Copyright © 2020 by the IEEE-ISTO Printer Working Group
 * Copyright © 2008-2018 by Apple Inc.
 *
 * Licensed under Apache License v2.0.  See the file "LICENSE" for more
 * information.
 */

/*
 * Include necessary headers.
 */

#define _CUPS_NO_DEPRECATED
#include <cups/cups-private.h>
#ifdef _WIN32
#  include <process.h>
#  include <sys/timeb.h>
#else
#  include <sys/wait.h>
#endif /* _WIN32 */
#include <regex.h>
#ifdef HAVE_MDNSRESPONDER
#  include <dns_sd.h>
#elif defined(HAVE_AVAHI)
#  include <avahi-client/client.h>
#  include <avahi-client/lookup.h>
#  include <avahi-common/simple-watch.h>
#  include <avahi-common/domain.h>
#  include <avahi-common/error.h>
#  include <avahi-common/malloc.h>
#  define kDNSServiceMaxDomainName AVAHI_DOMAIN_NAME_MAX
#endif /* HAVE_MDNSRESPONDER */

#ifndef _WIN32
extern char **environ;			/* Process environment variables */
#endif /* !_WIN32 */


/*
 * Structures...
 */

typedef enum ippfind_exit_e		/* Exit codes */
{
  IPPFIND_EXIT_TRUE = 0,		/* OK and result is true */
  IPPFIND_EXIT_FALSE,			/* OK but result is false*/
  IPPFIND_EXIT_BONJOUR,			/* Browse/resolve failure */
  IPPFIND_EXIT_SYNTAX,			/* Bad option or syntax error */
  IPPFIND_EXIT_MEMORY			/* Out of memory */
} ippfind_exit_t;

typedef enum ippfind_op_e		/* Operations for expressions */
{
  /* "Evaluation" operations */
  IPPFIND_OP_NONE,			/* No operation */
  IPPFIND_OP_AND,			/* Logical AND of all children */
  IPPFIND_OP_OR,			/* Logical OR of all children */
  IPPFIND_OP_TRUE,			/* Always true */
  IPPFIND_OP_FALSE,			/* Always false */
  IPPFIND_OP_IS_LOCAL,			/* Is a local service */
  IPPFIND_OP_IS_REMOTE,			/* Is a remote service */
  IPPFIND_OP_DOMAIN_REGEX,		/* Domain matches regular expression */
  IPPFIND_OP_NAME_REGEX,		/* Name matches regular expression */
  IPPFIND_OP_NAME_LITERAL,		/* Name matches literal string */
  IPPFIND_OP_HOST_REGEX,		/* Hostname matches regular expression */
  IPPFIND_OP_PORT_RANGE,		/* Port matches range */
  IPPFIND_OP_PATH_REGEX,		/* Path matches regular expression */
  IPPFIND_OP_TXT_EXISTS,		/* TXT record key exists */
  IPPFIND_OP_TXT_REGEX,			/* TXT record key matches regular expression */
  IPPFIND_OP_URI_REGEX,			/* URI matches regular expression */

  /* "Output" operations */
  IPPFIND_OP_EXEC,			/* Execute when true */
  IPPFIND_OP_LIST,			/* List when true */
  IPPFIND_OP_PRINT_NAME,		/* Print URI when true */
  IPPFIND_OP_PRINT_URI,			/* Print name when true */
  IPPFIND_OP_QUIET			/* No output when true */
} ippfind_op_t;

typedef struct ippfind_expr_s		/* Expression */
{
  struct ippfind_expr_s
		*prev,			/* Previous expression */
		*next,			/* Next expression */
		*parent,		/* Parent expressions */
		*child;			/* Child expressions */
  ippfind_op_t	op;			/* Operation code (see above) */
  int		invert;			/* Invert the result */
  char		*name;			/* TXT record key or literal name */
  regex_t	re;			/* Regular expression for matching */
  int		range[2];		/* Port number range */
  int		num_args;		/* Number of arguments for exec */
  char		**args;			/* Arguments for exec */
} ippfind_expr_t;

typedef struct ippfind_srv_s		/* Service information */
{
#ifdef HAVE_MDNSRESPONDER
  DNSServiceRef	ref;			/* Service reference for query */
#elif defined(HAVE_AVAHI)
  AvahiServiceResolver *ref;		/* Resolver */
#endif /* HAVE_MDNSRESPONDER */
  char		*name,			/* Service name */
		*domain,		/* Domain name */
		*regtype,		/* Registration type */
		*fullName,		/* Full name */
		*host,			/* Hostname */
		*resource,		/* Resource path */
		*uri;			/* URI */
  int		num_txt;		/* Number of TXT record keys */
  cups_option_t	*txt;			/* TXT record keys */
  int		port,			/* Port number */
		is_local,		/* Is a local service? */
		is_processed,		/* Did we process the service? */
		is_resolved;		/* Got the resolve data? */
} ippfind_srv_t;


/*
 * Local globals...
 */

#ifdef HAVE_MDNSRESPONDER
static DNSServiceRef dnssd_ref;		/* Master service reference */
#elif defined(HAVE_AVAHI)
static AvahiClient *avahi_client = NULL;/* Client information */
static int	avahi_got_data = 0;	/* Got data from poll? */
static AvahiSimplePoll *avahi_poll = NULL;
					/* Poll information */
#endif /* HAVE_MDNSRESPONDER */

static int	address_family = AF_UNSPEC;
					/* Address family for LIST */
static int	bonjour_error = 0;	/* Error browsing/resolving? */
static double	bonjour_timeout = 1.0;	/* Timeout in seconds */
static int	ipp_version = 20;	/* IPP version for LIST */


/*
 * Local functions...
 */

#ifdef HAVE_MDNSRESPONDER
static void DNSSD_API	browse_callback(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex, DNSServiceErrorType errorCode, const char *serviceName, const char *regtype, const char *replyDomain, void *context) _CUPS_NONNULL(1,5,6,7,8);
static void DNSSD_API	browse_local_callback(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex, DNSServiceErrorType errorCode, const char *serviceName, const char *regtype, const char *replyDomain, void *context) _CUPS_NONNULL(1,5,6,7,8);
#elif defined(HAVE_AVAHI)
static void		browse_callback(AvahiServiceBrowser *browser,
					AvahiIfIndex interface,
					AvahiProtocol protocol,
					AvahiBrowserEvent event,
					const char *serviceName,
					const char *regtype,
					const char *replyDomain,
					AvahiLookupResultFlags flags,
					void *context);
static void		client_callback(AvahiClient *client,
					AvahiClientState state,
					void *context);
#endif /* HAVE_MDNSRESPONDER */

static int		compare_services(ippfind_srv_t *a, ippfind_srv_t *b);
static const char	*dnssd_error_string(int error);
static int		eval_expr(ippfind_srv_t *service,
			          ippfind_expr_t *expressions);
static int		exec_program(ippfind_srv_t *service, int num_args,
			             char **args);
static ippfind_srv_t	*get_service(cups_array_t *services, const char *serviceName, const char *regtype, const char *replyDomain) _CUPS_NONNULL(1,2,3,4);
static double		get_time(void);
static int		list_service(ippfind_srv_t *service);
static ippfind_expr_t	*new_expr(ippfind_op_t op, int invert,
			          const char *value, const char *regex,
			          char **args);
#ifdef HAVE_MDNSRESPONDER
static void DNSSD_API	resolve_callback(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex, DNSServiceErrorType errorCode, const char *fullName, const char *hostTarget, uint16_t port, uint16_t txtLen, const unsigned char *txtRecord, void *context) _CUPS_NONNULL(1,5,6,9, 10);
#elif defined(HAVE_AVAHI)
static int		poll_callback(struct pollfd *pollfds,
			              unsigned int num_pollfds, int timeout,
			              void *context);
static void		resolve_callback(AvahiServiceResolver *res,
					 AvahiIfIndex interface,
					 AvahiProtocol protocol,
					 AvahiResolverEvent event,
					 const char *serviceName,
					 const char *regtype,
					 const char *replyDomain,
					 const char *host_name,
					 const AvahiAddress *address,
					 uint16_t port,
					 AvahiStringList *txt,
					 AvahiLookupResultFlags flags,
					 void *context);
#endif /* HAVE_MDNSRESPONDER */
static void		set_service_uri(ippfind_srv_t *service);
static void		show_usage(void) _CUPS_NORETURN;
static void		show_version(void) _CUPS_NORETURN;

//  global variables

static AvahiSimplePoll *simple_poll = NULL;

//  local functions 

static void		browse_callback(AvahiServiceBrowser *browser,
					AvahiIfIndex interface,
					AvahiProtocol protocol,
					AvahiBrowserEvent event,
					const char *serviceName,
					const char *regtype,
					const char *replyDomain,
					AvahiLookupResultFlags flags,
					void *context);
static void		client_callback(AvahiClient *client,
					AvahiClientState state,
					void *context);

                    
// individual functions for browse and resolve

/*
    implementation of avahi_intialize, to create objects necessary for 
    browse and resolve to work
    */

AvahiClient* avahi_intialize(){
    AvahiClient *client = NULL;
    
    /* allocate main loop object */
    if (!(simple_poll = avahi_simple_poll_new())) {
        fprintf(stderr, "Initialization Error, Failed to create simple poll object.\n");
        return NULL;
    }

    int error;
    /* allocate a new client */
    client = avahi_client_new(avahi_simple_poll_get(simple_poll), (AvahiClientFlags)0, client_callback, NULL, &error);

    if(!client){
        fprintf(stderr, "Initialization Error, Failed to create client: %s\n", avahi_strerror(error));
        return NULL;
    }

    return client;
}

void browse_services(char *regtype){
    //initialize a client object
    AvahiClient* client;

    if(!(client = avahi_intialize())){
        fprintf(stderr, "Initialization Error");
        return ;
    }

    AvahiServiceBrowser *sb = NULL;

    if (!(sb = avahi_service_browser_new(client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, regtype, NULL, (AvahiLookupFlags)0, browse_callback, client))) {
        fprintf(stderr, "Failed to create service browser: %s\n", avahi_strerror(avahi_client_errno(client)));
        return ;
    }
    
}
void resolve_services();

/*
 * 'browse_callback()' - Browse devices.
 */

static void
browse_callback(
    AvahiServiceBrowser    *browser,	/* I - Browser */
    AvahiIfIndex           interface,	/* I - Interface index (unused) */
    AvahiProtocol          protocol,	/* I - Network protocol (unused) */
    AvahiBrowserEvent      event,	/* I - What happened */
    const char             *name,	/* I - Service name */
    const char             *type,	/* I - Registration type */
    const char             *domain,	/* I - Domain */
    AvahiLookupResultFlags flags,	/* I - Flags */
    void                   *context)	/* I - Services array */
{
  AvahiClient	*client = avahi_service_browser_get_client(browser);
					/* Client information */
  ippfind_srv_t	*service;		/* Service information */


  (void)interface;
  (void)protocol;
  (void)context;

  switch (event)
  {
    case AVAHI_BROWSER_FAILURE:
	fprintf(stderr, "DEBUG: browse_callback: %s\n",
		avahi_strerror(avahi_client_errno(client)));
	bonjour_error = 1;
	avahi_simple_poll_quit(avahi_poll);
	break;

    case AVAHI_BROWSER_NEW:
       /*
	* This object is new on the network. Create a device entry for it if
	* it doesn't yet exist.
	*/

	service = get_service((cups_array_t *)context, name, type, domain);

	if (flags & AVAHI_LOOKUP_RESULT_LOCAL)
	  service->is_local = 1;
	break;

    case AVAHI_BROWSER_REMOVE:
    case AVAHI_BROWSER_ALL_FOR_NOW:
    case AVAHI_BROWSER_CACHE_EXHAUSTED:
        break;
  }
}


/*
 * 'client_callback()' - Avahi client callback function.
 */

static void
client_callback(
    AvahiClient      *client,		/* I - Client information (unused) */
    AvahiClientState state,		/* I - Current state */
    void             *context)		/* I - User data (unused) */
{
  (void)client;
  (void)context;

 /*
  * If the connection drops, quit.
  */

  if (state == AVAHI_CLIENT_FAILURE)
  {
    fputs("DEBUG: Avahi connection failed.\n", stderr);
    bonjour_error = 1;
    avahi_simple_poll_quit(avahi_poll);
  }
}

