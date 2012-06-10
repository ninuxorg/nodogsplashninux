/********************************************************************\
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652       *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                   *
 *                                                                  *
 \********************************************************************/

/* $Id: http.c 1104 2006-10-09 00:58:46Z acv $ */
/** @file http.c
  @brief HTTP IO functions
  @author Copyright (C) 2004 Philippe April <papril777@yahoo.com>
  @author Copyright (C) 2007 Paul Kube <nodogsplash@kokoro.ucsd.edu>
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include "httpd.h"

#include "safe.h"
#include "debug.h"
#include "conf.h"
#include "auth.h"
#include "firewall.h"
#include "http.h"
#include "httpd.h"
#include "client_list.h"
#include "common.h"

#include "util.h"

#include "../config.h"

extern pthread_mutex_t	client_list_mutex;


void 
http_callback_about(httpd *webserver, request *r) {
  http_nodogsplash_serve_info(r, "Nodogsplash Info",
			      "This is Nodogsplash version <b>" VERSION "</b>");
}

void 
http_callback_status(httpd *webserver, request *r) {
  char * status = NULL;
  char * prestatus = NULL;
  status = get_status_text();
  safe_asprintf(&prestatus, "<pre>\n%s\n</pre>", status);
  http_nodogsplash_serve_info(r, "Nodogsplash Status",prestatus);
  free(status);
  free(prestatus);
}


/** The 404 handler is one way a client can first hit nodogsplash.
 */
void
http_nodogsplash_callback_404(httpd *webserver, request *r) {
  
  debug(LOG_INFO, "Capturing as 404 request from %s for [%s%s]",
	r->clientAddr, r->request.host, r->request.path);

  http_nodogsplash_first_contact(r);
}


/** The index handler is one way a client can first hit nodogsplash.
 */
void 
http_nodogsplash_callback_index(httpd *webserver, request *r) {

  debug(LOG_INFO, "Capturing index request from %s for [%s%s]",
	r->clientAddr, r->request.host, r->request.path);

  http_nodogsplash_first_contact(r);
}

/** Respond to attempted access from a preauthenticated client.
 *  Add the client to the client list and serves the splash page.
 */
void 
http_nodogsplash_first_contact(request *r) {
  t_client *client;
  t_auth_target *authtarget;
  s_config *config;
  char *redir;

  config = config_get_config();

  client = http_nodogsplash_add_client(r);
  /* http_nodogsplash_add_client() should log and return null on error */
  if(!client) return;
  
  /* Create redirect URL for this contact as appropriate */
  redir = http_nodogsplash_make_redir(r->request.host,r->request.path);
  /* Create authtarget with all needed info */
  authtarget = http_nodogsplash_make_authtarget(client->token,redir);
  free(redir);

  if(config->authenticate_immediately) {
    /* Don't serve splash, just authenticate */
    http_nodogsplash_callback_action(r,authtarget,AUTH_MAKE_AUTHENTICATED);
  } else {
    /* Serve the splash page (or redirect to remote authenticator) */
    http_nodogsplash_serve_splash(r,authtarget);
  }

  http_nodogsplash_free_authtarget(authtarget);

}



/** The multipurpose authentication action handler
 */
void
http_nodogsplash_callback_action(request *r,
				 t_auth_target *authtarget,
				 t_authaction action) {
  t_client	*client;
  char *mac;
  char *ip;
  char *clienttoken = NULL;
  char *requesttoken = authtarget->token;
  char *redir = authtarget->redir;
  s_config *config;

  config = config_get_config();
  
  ip = r->clientAddr;

  if(!requesttoken) {
    debug(LOG_NOTICE, "No token in request from ip %s", ip);
    return;
  }
  if(!redir) {
    debug(LOG_NOTICE, "No redirect in request from ip %s", ip);
    return;
  }

  if (!(mac = arp_get(ip))) {
    /* We could not get their MAC address */
    debug(LOG_NOTICE, "Could not arp MAC address for %s action %d", ip, action);
    return;
  }

  /* We have their MAC address, find them on the client list */
  LOCK_CLIENT_LIST();
  client = client_list_find(ip,mac);
  if(client && client->token) {
    clienttoken = safe_strdup(client->token);
  }
  UNLOCK_CLIENT_LIST();

  if(!client) {
    debug(LOG_NOTICE, "Client %s %s action %d is not on client list",
	  ip, mac, action);
    http_nodogsplash_serve_info(r,
				"Nodogsplash Error",
				"You are not on the client list.");
    free(mac);
    return;
  }

  /* We have a client */

  /* Do we have a client token? */
  if(!clienttoken) {
    debug(LOG_NOTICE, "Client %s %s action %d does not have a token",
	  ip, mac, action);
    free(mac);
    return;
  } 

  debug(LOG_DEBUG, "Action %d: %s %s tokens %s, %s",
	action, ip, mac, clienttoken, requesttoken);  
  debug(LOG_DEBUG, "Redirect:  %s", redir);

  /* Check token match */
  if (strcmp(clienttoken,requesttoken)) {
    /* tokens don't match, reject */
    debug(LOG_NOTICE, "Client %s %s tokens %s, %s do not match",
	  r->clientAddr, mac, clienttoken, requesttoken);
    http_nodogsplash_serve_info(r, "Nodogsplash Error",
				"Tokens do not match.");
    free(mac);
    free(clienttoken);
    return;
  }

  /* Log value of info string, if any */
  if(authtarget->info) {
    debug(LOG_NOTICE, "Client %s %s info: %s",
	  ip, mac, authtarget->info);
  }
    

  /* take action */
  switch(action) {
  case AUTH_MAKE_AUTHENTICATED:
    auth_client_action(ip,mac,action);
    http_nodogsplash_redirect(r, redir);
    break;
  case AUTH_MAKE_DEAUTHENTICATED:
    auth_client_action(ip,mac,action);
    http_nodogsplash_serve_info(r, "Nodogsplash Deny",
				"Authentication revoked.");
    break;
  default:
    debug(LOG_ERR, "Unknown auth action: %d", action);
  }
   
  free(mac);
  free(clienttoken);
  return;

}

/** The auth callback responds to a request to serve from the authdir */
void
http_nodogsplash_callback_auth(httpd *webserver, request *r) {
  t_auth_target *authtarget;
  
  /* Get info we need from request, and do action */
  authtarget = http_nodogsplash_decode_authtarget(r);

  if(http_nodogsplash_check_userpass(r,authtarget)) {
    http_nodogsplash_callback_action (r,authtarget,AUTH_MAKE_AUTHENTICATED);
  } else {
    /* Password check failed; just serve them the splash page again */
    http_nodogsplash_serve_splash(r,authtarget);
  }
  
  http_nodogsplash_free_authtarget(authtarget);
}


/** The deny callback responds to a request to serve from the denydir */
void
http_nodogsplash_callback_deny(httpd *webserver, request *r) {
  t_auth_target *authtarget;

  /* Get info we need from request, and do action */
  authtarget = http_nodogsplash_decode_authtarget(r);
  http_nodogsplash_callback_action (r,authtarget,AUTH_MAKE_DEAUTHENTICATED );
  http_nodogsplash_free_authtarget(authtarget);
}



/**
 *  Add client making a request to client list.
 *  Return pointer to the client list entry for this client.
 *
 *  N.B.: This does not authenticate the client; it only makes
 *  their information available on the client list.
 */
t_client *
http_nodogsplash_add_client(request *r) {
  t_client	*client;  
  LOCK_CLIENT_LIST();
  client = client_list_add_client(r->clientAddr);
  UNLOCK_CLIENT_LIST();
  return client;
}
 


void
http_nodogsplash_redirect_remote_auth(request *r, t_auth_target *authtarget) {
  char *remoteurl;
  char *encgateway, *encauthaction, *encredir, *enctoken;
  s_config	*config;

  config = config_get_config();

  /* URL encode variables, redirect to remote auth server */
  encgateway = httpdUrlEncode(config->gw_name);
  encauthaction = httpdUrlEncode(authtarget->authaction);
  encredir = httpdUrlEncode(authtarget->redir);
  enctoken = httpdUrlEncode(authtarget->token);
  safe_asprintf(&remoteurl, "%s?gateway=%s&authaction=%s&redir=%s&tok=%s",
		config->remote_auth_action,
		encgateway,
		encauthaction,
		encredir,
		enctoken);
  http_nodogsplash_redirect(r, remoteurl);
  free(encgateway);
  free(encauthaction);
  free(encredir);
  free(enctoken);
  free(remoteurl);
}

/* Pipe the splash page from the splash page file,
 * or redirect to remote authenticator as required.
 */
void
http_nodogsplash_serve_splash(request *r, t_auth_target *authtarget) {
  char *tmpstr;
  char line [MAX_BUF];
  char *splashfilename;
  FILE *fd;
  s_config	*config;

  
  config = config_get_config();

  if(config->remote_auth_action) {
    /* Redirect to remote auth server instead of serving local splash page */
    http_nodogsplash_redirect_remote_auth(r, authtarget);
    return;
  }

  /* Set variables; these can be interpolated in the splash page text. */
  httpdAddVariable(r,"gatewayname",config->gw_name);
  httpdAddVariable(r,"tok",authtarget->token);
  httpdAddVariable(r,"redir",authtarget->redir);
  httpdAddVariable(r,"authaction",authtarget->authaction);
  httpdAddVariable(r,"denyaction",authtarget->denyaction);
  httpdAddVariable(r,"authtarget",authtarget->authtarget);
  httpdAddVariable(r,"clientip",r->clientAddr);
  safe_asprintf(&tmpstr, "%d", get_client_list_length());
  httpdAddVariable(r,"nclients",tmpstr);
  free(tmpstr);
  safe_asprintf(&tmpstr, "%d", config->maxclients);
  httpdAddVariable(r,"maxclients",tmpstr);
  free(tmpstr);
  tmpstr = get_uptime_string();
  httpdAddVariable(r,"uptime",tmpstr);
  free(tmpstr);
  /* We need to have imagesdir and pagesdir appear in the page
     as absolute paths, so they work no matter what the
     initial user request URL was  */
  safe_asprintf(&tmpstr, "/%s", config->imagesdir);
  httpdAddVariable(r,"imagesdir",tmpstr);
  free(tmpstr);
  safe_asprintf(&tmpstr, "/%s", config->pagesdir);
  httpdAddVariable(r,"pagesdir",tmpstr);
  free(tmpstr);
  

  /* Pipe the splash page from its file */
  safe_asprintf(&splashfilename, "%s/%s", config->webroot, config->splashpage );
  debug(LOG_INFO,"Serving splash page %s to %s",
	splashfilename,r->clientAddr);
  if (!(fd = fopen(splashfilename, "r"))) {
    debug(LOG_ERR, "Could not open splash page file '%s'", splashfilename);
    http_nodogsplash_serve_info(r, "Nodogsplash Error",
				"Failed to open splash page");
  } else {
    while (fgets(line, MAX_BUF, fd)) {
      httpdOutput(r,line);
    }
    fclose(fd);
  }

  free(splashfilename);

}

/* Pipe the info page from the info skeleton page file.
 */
void
http_nodogsplash_serve_info(request *r, char *title, char *content) {
  char *abspath;
  char line [MAX_BUF];
  char *infoskelfilename;
  FILE *fd;
  s_config	*config;
  
  config = config_get_config();

  /* Set variables; these can be interpolated in the info page text. */
  httpdAddVariable(r,"gatewayname",config->gw_name);
  httpdAddVariable(r,"version",VERSION);
  httpdAddVariable(r,"title",title);
  httpdAddVariable(r,"content",content);
  /* We need to have imagesdir and pagesdir appear in the page
     as absolute paths, so they work no matter what the
     initial user request URL was  */
  safe_asprintf(&abspath, "/%s", config->imagesdir);
  httpdAddVariable(r,"imagesdir",abspath);
  free(abspath);
  safe_asprintf(&abspath, "/%s", config->pagesdir);
  httpdAddVariable(r,"pagesdir",abspath);
  free(abspath);
  
  /* Pipe the page from its file */
  safe_asprintf(&infoskelfilename, "%s/%s",
		config->webroot,
		config->infoskelpage );
  debug(LOG_INFO,"Serving info page %s title %s to %s",
	infoskelfilename,r->clientAddr);
  if (!(fd = fopen(infoskelfilename, "r"))) {
    debug(LOG_ERR, "Could not open info skel file '%s'", infoskelfilename);
  } else {
    while (fgets(line, MAX_BUF, fd)) {
      httpdOutput(r,line);
    }
    fclose(fd);
  }
  free(infoskelfilename);
}



void
http_nodogsplash_redirect(request *r, char *url) {
  char *header;

  httpdSetResponse(r, "307 Temporary Redirect");
  safe_asprintf(&header, "Location: %s",url);
  httpdAddHeader(r, header);

  httpdPrintf(r, "<html><head></head><body><a href='%s'>Click here to continue to<br>%s</a></body></html>",url,url);

  free(header);

}

/**
 * Allocate and return a pointer to a t_auth_target struct
 * encoding information needed to authenticate a client.
 * See http_nodogsplash_make_authtarget().
 * The struct should be freed by http_nodogsplash_free_authtarget().
 */
t_auth_target *
http_nodogsplash_decode_authtarget(request *r) {
  httpVar *var;
  t_auth_target *authtarget;
  char *token=NULL, *redir=NULL;
  

  var = httpdGetVariableByName(r,"tok");
  if(var && var->value) {
    token = var->value;
  } else {
    token = "";
  }

  var = httpdGetVariableByName(r,"redir");
  if(var && var->value) {
    redir = var->value;
  } else {
    redir = "";
  }

  authtarget = http_nodogsplash_make_authtarget(token,redir);

  var = httpdGetVariableByName(r,"nodoguser");
  if(var && var->value) {
    authtarget->username = safe_strdup(var->value);
  }
  var = httpdGetVariableByName(r,"nodogpass");
  if(var && var->value) {
    authtarget->password = safe_strdup(var->value);
  }
  var = httpdGetVariableByName(r,"info");
  if(var && var->value) {
    authtarget->info = safe_strdup(var->value);
  }


  return authtarget;

}


/* Allocate and return a pointer to a string that is the redirect URL.
 * Caller must free.
 */
char*
http_nodogsplash_make_redir(char* redirhost, char* redirpath) {
  s_config *config;
  char* redir;
  config = config_get_config();

  if(config->redirectURL) {
    debug(LOG_DEBUG,"Redirect request http://%s%s, substituting %s",
	  redirhost,redirpath,config->redirectURL);
    redir = safe_strdup(config->redirectURL);
  } else {
    /* We just assume protocol http; after all we caught the client by
       redirecting port 80 tcp packets
    */
    safe_asprintf(&redir,"http://%s%s",redirhost,redirpath);
  }
  return redir;
}

/**
 * Allocate and return a pointer to a t_auth_target struct encoding information
 * needed to eventually authenticate a client.
 * The struct should be freed by http_nodogsplash_free_authtarget().
 */
t_auth_target*
http_nodogsplash_make_authtarget(char* token, char* redir) {
  char *encodedredir;
  char *encodedtok;
  t_auth_target* authtarget;
  s_config *config;

  config = config_get_config();

  authtarget = safe_malloc(sizeof(t_auth_target));
  memset(authtarget, 0, sizeof(t_auth_target));

  authtarget->ip = safe_strdup(config->gw_address);
  authtarget->port = config->gw_port;
  authtarget->authdir = safe_strdup(config->authdir);
  authtarget->denydir = safe_strdup(config->denydir);
  safe_asprintf(&(authtarget->authaction),"http://%s:%d/%s/",authtarget->ip,authtarget->port,authtarget->authdir);
  safe_asprintf(&(authtarget->denyaction),"http://%s:%d/%s/",authtarget->ip,authtarget->port,authtarget->denydir);
  authtarget->token = safe_strdup(token);
  authtarget->redir = safe_strdup(redir);
  encodedredir = httpdUrlEncode(authtarget->redir);  /* malloc's */
  encodedtok = httpdUrlEncode(authtarget->token);  /* malloc's */
  safe_asprintf(&(authtarget->authtarget), "%s?redir=%s&tok=%s",
		authtarget->authaction,
		encodedredir,
		encodedtok);
  free(encodedredir);
  free(encodedtok);

  return authtarget;
}


void
http_nodogsplash_free_authtarget(t_auth_target* authtarget) {

  if(authtarget->ip) free(authtarget->ip);
  if(authtarget->authdir) free(authtarget->authdir);
  if(authtarget->denydir) free(authtarget->denydir);
  if(authtarget->authaction) free(authtarget->authaction);
  if(authtarget->denyaction) free(authtarget->denyaction);
  if(authtarget->authtarget) free(authtarget->authtarget);
  if(authtarget->token) free(authtarget->token);
  if(authtarget->redir) free(authtarget->redir);
  if(authtarget->username) free(authtarget->username);
  if(authtarget->password) free(authtarget->password);
  if(authtarget->info) free(authtarget->info);
  free(authtarget);

}

/** Perform username/password check if configured to use it.
 */
int
http_nodogsplash_check_userpass(request *r, t_auth_target *authtarget) {
  s_config *config;
  t_client  *client;
  config = config_get_config();
  int attempts = 0;
  char *ip;
  char *mac;

  if(!config->passwordauth && !config->usernameauth) {
    /* Not configured to use username/password check; can't fail. */
    return 1;
  }

  ip = r->clientAddr;

  if (!(mac = arp_get(ip))) {
    /* we could not get their MAC address; fail */
    debug(LOG_NOTICE, "Could not arp MAC address for %s to check user/password", ip);
    return 0;
  }

  /* We have their MAC address, find them on the client list
     and increment their password attempt counter */
  LOCK_CLIENT_LIST();
  client = client_list_find(ip,mac);
  if(client) attempts = ++(client->attempts);
  UNLOCK_CLIENT_LIST();

  if(!client) {
    /* not on client list; fail */
    debug(LOG_NOTICE, "Client %s %s not on client list to check user/password",
	  ip, mac);
    free(mac);
    return 0;
  }
  
  if(attempts > config->passwordattempts) {
    /* too many attempts; fail */
    debug(LOG_NOTICE, "Client %s %s exceeded %d password attempts",
	  ip, mac, config->passwordattempts);
    free(mac);
    return 0;
  }

  if ((!config->usernameauth ||
       authtarget->username && !strcmp(config->username,authtarget->username))
      &&
      (!config->passwordauth ||
       authtarget->password && !strcmp(config->password,authtarget->password))) {
    /* password and username match; success */
    debug(LOG_NOTICE, "Client %s %s username/password '%s'/'%s'",
	  ip, mac,
	  authtarget->username,
	  authtarget->password);
    free(mac);
    return 1;
  }

  /* fail */
  debug(LOG_NOTICE, "Client %s %s bad username/password '%s'/'%s'",
	ip, mac,
	authtarget->username,
	authtarget->password);
  free(mac);
  return 0;

}

