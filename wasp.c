// Web Application Support Platform
// Control command
// (c) Adrian Kennard Andrews & Arnold 2018

#include <stdio.h>
#include <string.h>
#include <popt.h>
#include <time.h>
#include <sys/time.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctype.h>
#include <err.h>
#include <signal.h>
#include <curl/curl.h>
#include <axl.h>

// Main config
#define xquoted(x)      #x
#define quoted(x)       xquoted(x)
#ifdef  PORT
char *port = quoted (PORT);
#else
char *port = "8443";
#endif
#ifdef	TLS
int tls = 1;
#else
int tls = 0;
#endif

int debug = 0;

int
main (int argc, const char *argv[])
{
   int exists = 0;
   int add = 0;
   int remove = 0;
   int clear = 0;
   int disconnect = 0;
   const char *target = NULL;
   const char *jsonfile = NULL;
   const char *sendfile = NULL;
   {                            // defaults for running from server on alternate port
      char *waspport = getenv ("WASPPORT");
      if (waspport)
      {
         char *p = strrchr (waspport, '#');
         if (p)
            waspport = p + 1;
         port = waspport;
      }
   }
   poptContext optCon;          // context for parsing command-line options
   {                            // POPT
      const struct poptOption optionsTable[] = {
         {"target", 't', POPT_ARG_STRING, &target, 0, "Target", "channel/id"},
         {"json", 'j', POPT_ARG_STRING, &jsonfile, 0, "JSON send", "filename"},
         {"send", 's', POPT_ARG_STRING, &sendfile, 0, "Raw Send", "filename"},
         {"exists", 'e', POPT_ARG_NONE, &exists, 0, "Exists flag", NULL},
         {"disconnect", 'd', POPT_ARG_NONE, &disconnect, 0, "Send disconnect", NULL},
         {"add", 'a', POPT_ARG_NONE, &add, 0, "Add to channels", NULL},
         {"remove", 'r', POPT_ARG_NONE, &remove, 0, "Remove from channels", NULL},
         {"clear", 'c', POPT_ARG_NONE, &clear, 0, "Clear channels", NULL},
         {"port", 'p', POPT_ARG_STRING | (port ? POPT_ARGFLAG_SHOW_DEFAULT : 0), &port, 0, "Port", "port"},
         {"tls", 0, POPT_ARG_NONE | (tls ? POPT_ARGFLAG_DOC_HIDDEN : 0), &tls, 0, "Use TLS", NULL},
         {"debug", 'v', POPT_ARG_NONE, &debug, 0, "Debug", NULL},
         POPT_AUTOHELP {}
      };

      optCon = poptGetContext (NULL, argc, argv, optionsTable, 0);
      poptSetOtherOptionHelp (optCon, "[Channels]/[name/vars]");

      int c;
      if ((c = poptGetNextOpt (optCon)) < -1)
         errx (1, "%s: %s\n", poptBadOption (optCon, POPT_BADOPTION_NOALIAS), poptStrerror (c));

      if (!target && poptPeekArg (optCon))
         target = poptGetArg (optCon);
      if (!target || (jsonfile && sendfile) || (add && remove) || (clear && remove)
          || ((add || remove || clear) && (jsonfile || sendfile)))
      {
         poptPrintUsage (optCon, stderr, 0);
         return -1;
      }
   }
   CURL *curl = curl_easy_init ();
   if (debug)
      curl_easy_setopt (curl, CURLOPT_VERBOSE, 1L);
   if (tls)
   {                            // Cannot validate localhost
      curl_easy_setopt (curl, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt (curl, CURLOPT_SSL_VERIFYHOST, 0L);
   }
   char *request = NULL;
   size_t requestlen = 0;
   FILE *o = open_memstream (&request, &requestlen);
   char *url = NULL;
   if (asprintf
       (&url, "http%s://localhost:%s/%s%s?%s", tls ? "s" : "", port, exists ? "*" : "",
        clear ? "clear" : add ? "add" : remove ? "remove" : disconnect ? "disconnect" : "message", target) < 0)
      errx (1, "malloc");
   if (add || remove || clear)
   {                            // Channels
      while (poptPeekArg (optCon))
         fprintf (o, "%s\n", poptGetArg (optCon));
   } else
   {                            // Message
      if (sendfile)
      {                         // Send raw file
         int f = fileno (stdin);
         if (strcmp (sendfile, "-"))
            f = open (sendfile, O_RDONLY);
         if (f < 0)
            err (1, "Cannot open %s", sendfile);
         char buf[1024];
         size_t p;
         while ((p = read (f, buf, sizeof (buf))) > 0)
            fwrite (buf, p, 1, o);
         if (f != fileno (stdin))
            close (f);
      } else if (jsonfile || poptPeekArg (optCon))
      {
         xml_t xml = NULL;
         if (jsonfile)
         {                      // Send JSON
            FILE *f = stdin;
            if (strcmp (jsonfile, "-"))
               f = fopen (jsonfile, "r");
            if (!f)
               err (1, "Cannot open %s", jsonfile);
            xml = xml_tree_read_json (f, NULL);
            if (!xml)
            {
               rewind (f);      // Messy for stdin
               xml = xml_tree_read (f);
            }
            if (!xml)
               errx (1, "Cannot read/parse %s", jsonfile);
            if (f != stdin)
               fclose (f);
         } else
            xml = xml_tree_new ("json");
         if (poptPeekArg (optCon))
         {                      // Construct message
            while (poptPeekArg (optCon))
            {
               char *c = strdupa (poptGetArg (optCon));
               char *d = c;
               if (isalpha (*d) || *d == '_')
                  while (isalnum (*d) || *d == '_')
                     d++;
               if (*d == '=')
               {                // Name=Value
                  *d++ = 0;
                  xml_t e = xml_element_add (xml, c);
                  e->json_single = 1;
                  xml_element_set_content (e, d);
               } else if (*d == '#')
               {                // Name=Value (unquoted) e.g. numeric, boolean
                  *d++ = 0;
                  if (*d == '$')
                     d = getenv (d + 1);        // Environment variable content
                  if (d)
                  {
                     xml_t e = xml_element_add (xml, c);
                     e->json_single = 1;
                     e->json_unquoted = 1;
                     xml_element_set_content (e, d);
                  }
               } else if (*d == '$')
               {                // Name=$variable
                  *d++ = 0;
                  char *v = getenv (d);
                  if (v)
                  {
                     xml_t e = xml_element_add (xml, c);
                     e->json_single = 1;
                     xml_element_set_content (e, v);
                  }
               } else if (*d == '@')
               {                // Name string from filename
                  *d++ = 0;
                  int f = fileno (stdin);
                  if (strcmp (d, "-"))
                     f = open (d, O_RDONLY);
                  if (f < 0)
                     err (1, "Cannot open %s", d);
                  char *val = NULL;
                  size_t len = 0;
                  FILE *o = open_memstream (&val, &len);
                  char buf[1024];
                  size_t p;
                  while ((p = read (f, buf, sizeof (buf))) > 0)
                     fwrite (buf, p, 1, o);
                  close (f);
                  fclose (o);
                  if (val)
                  {
                     xml_t e = xml_element_add (xml, c);
                     e->json_single = 1;
                     xml_element_set_content (e, val);
                     free (val);
                  }
               } else if (*d == ':')
               {                // Name object from filename
                  *d++ = 0;
                  xml_t a = xml_tree_read_file (d);
                  if (!a)
                     a = xml_tree_read_file_json (d);
                  if (!a)
                     err (1, "Cannot read %s", d);
                  xml_element_set_name (a, c);
                  xml_t e = xml_element_attach (xml, a);
                  e->json_single = 1;
                  xml_tree_delete (a);
               } else if (!*d)
               {                // Name as variable
                  char *v = getenv (c);
                  if (v)
                  {
                     xml_t e = xml_element_add (xml, c);
                     e->json_single = 1;
                     xml_element_set_content (e, v);
                     if (!strcmp (v, "true") || !strcmp (v, "false") || !strcmp (v, "null"))
                        e->json_unquoted = 1;
                     else if (*v)
                     {
                        if (*v == '+' || *v == '-')
                           v++;
                        while (isdigit (*v))
                           v++;
                        if (*v == '.')
                           v++;
                        while (isdigit (*v))
                           v++;
                        if (*v == 'e' || *v == 'E')
                        {
                           v++;
                           if (*v == '+' || *v == '-')
                              v++;
                           while (isdigit (*v))
                              v++;
                        }
                        if (!*v)
                           e->json_unquoted = 1;
                     }
                  }

               } else
                  errx (1, "Unknown arg [%s]", c);
            }
         }
         xml_write_json (o, xml);
         xml_tree_delete (xml);
      }
   }
   fclose (o);
   if (requestlen)
   {
      if (debug)
         fprintf (stderr, "[%.*s]\n", (int) requestlen, request);
      curl_easy_setopt (curl, CURLOPT_POSTFIELDS, request);
      curl_easy_setopt (curl, CURLOPT_POSTFIELDSIZE, (long) requestlen);
      curl_easy_setopt (curl, CURLOPT_POST, 1L);
   } else if (!disconnect && !clear)
      errx (1, "No message/groups");
   char *reply = NULL;
   size_t replylen = 0;
   FILE *r = open_memstream (&reply, &replylen);
   curl_easy_setopt (curl, CURLOPT_WRITEDATA, r);
   curl_easy_setopt (curl, CURLOPT_URL, url);
   CURLcode result = curl_easy_perform (curl);
   fclose (r);
   long code = 0;
   if (!result)
      curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &code);
   if (code / 100 == 2)
      code = 0;
   if (code)
      warnx ("Error response %d (%s)", (int) code, reply ? : "");
   if (reply)
      free (reply);
   if (request)
      free (request);
   curl_easy_cleanup (curl);
   poptFreeContext (optCon);
   return code ? 1 : 0;
}
