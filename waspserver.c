// Web Application Support Platform
// Server
// (c) Adrian Kennard Andrews & Arnold 2018

#include <stdio.h>
#include <string.h>
#include <popt.h>
#include <time.h>
#include <sys/time.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <poll.h>
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
#include <websocket.h>
#include <pthread.h>

#define	RANDOM	"/dev/urandom"

// Main config
#define xquoted(x)      #x
#define quoted(x)       xquoted(x)
#ifdef  BINDING
char *binding = quoted (BINDING);
#else
char *binding = "8443";
#endif
#ifdef	CERT
char *certfile = quoted (CERT);
#else
char *certfile = NULL;
#endif
#ifdef	KEY
char *keyfile = quoted (KEY);
#else
char *keyfile = NULL;
#endif
#ifdef	CONNECT
extern int CONNECT (const char *sid, xml_t head);
#endif
#ifdef	CONNECTSCRIPT
char *connectscript = quoted (CONNECTSCRIPT);
#else
char *connectscript = NULL;
#endif
#ifdef	MESSAGE
extern int MESSAGE (const char *sid, size_t len, const unsigned char *data);
#endif
#ifdef	MESSAGESCRIPT
char *messagescript = quoted (MESSAGESCRIPT);
#else
char *messagescript = NULL;
#endif
#ifdef	DISCONNECT
extern int DISCONNECT (const char *sid, const char *id);
#endif
#ifdef	DISCONNECTSCRIPT
char *disconnectscript = quoted (DISCONNECTSCRIPT);
#else
char *disconnectscript = NULL;
#endif

int statchannel = 0;
int statsession = 0;
int statlink = 0;
int statqueue = 0;

// Types
typedef struct q_s q_t;
typedef struct wasp_session_s wasp_session_t;
typedef struct wasp_channel_s wasp_channel_t;
typedef struct wasp_chanlink_s wasp_chanlink_t;

int qpipe[2] = { };

struct q_s
{                               // Simple queue
   volatile q_t *next;
   wasp_session_t *session;     // Session
   xml_t head;                  // Head
   size_t len;                  // data len
   const unsigned char *data;   // data
};

struct wasp_session_s
{                               // WASP session
   wasp_session_t **prev,
    *next;                      // Sessions
   websocket_t *ws;             // The associated web socket
   char sid[21];                // Socket ID
   char *id;                    // Allocated connection ID
   wasp_chanlink_t *channels;   // Channels of which this session is part
};

struct wasp_channel_s
{
   wasp_channel_t **prev,
    *next;                      // Channels
   char *cid;                   // Channel name
   wasp_chanlink_t *sessions;   // Sessions on this channel
   int count;                   // Active slots
   int max;                     // Space allocated for ws
   websocket_t **ws;            // Websockets associated with channel
};

struct wasp_chanlink_s
{                               // Link a channel to a session
   wasp_chanlink_t **cprev,
    *cnext;                     // Chain of channel sessions
   wasp_chanlink_t **sprev,
    *snext;                     // Chain of session channels
   wasp_session_t *session;     // The session
   wasp_channel_t *channel;     // The channel
   int slot;                    // Which slot in ws list on channel
   char zap:1;                  // Being zapped
};

// Global variables
int debug = 0;
pthread_mutex_t mutex;          // main message mutex
pthread_mutex_t sessionmutex;   // Session update mutex
wasp_session_t *sessions = NULL;
wasp_channel_t *channels = NULL;
pthread_mutex_t qmutex;         // Queue control mutex
volatile q_t *queue = NULL,
   *queueend = NULL;

// functions

static char *
tokens (char *s, unsigned char n)
{                               // Generates a text token of specified number of characters, and null terminator
   if (!s || !n)
      return s;                 // NULL
   int f = open (RANDOM, O_RDONLY);
   if (f < 0)
      err (1, "Cannot open %s", RANDOM);
   if (read (f, s, n) != n)
      err (1, "Bad read random");
   int r;
   for (r = 0; r < n; r++)
      s[r] = BASE32[s[r] & 31];
   s[r] = 0;
   close (f);
   return s;
}

static wasp_channel_t *
find_channel (const char *cid)
{                               // Find a channel, maybe make some hash logic some time
   wasp_channel_t *c;
   for (c = channels; c && strcmp (c->cid, cid); c = c->next);
   return c;
}

static wasp_channel_t *
make_channel (const char *cid)
{
   wasp_channel_t *c = find_channel (cid);
   if (c)
      return c;
   c = malloc (sizeof (*c));
   if (!c)
      errx (1, "malloc");
   statchannel++;
   memset (c, 0, sizeof (*c));
   c->cid = strdup (cid);
   c->prev = &channels;
   c->next = channels;
   if (channels)
      channels->prev = &c->next;
   channels = c;
   if (debug)
      warnx ("%s Channel created", cid);
   return c;
}

static void
link_channel (wasp_session_t * s, wasp_channel_t * c)
{
   if (debug)
      warnx ("%s Link session %s", c->cid, s->sid);
   pthread_mutex_lock (&sessionmutex);
   wasp_chanlink_t *l = NULL;
   for (l = s->channels; l && l->channel != c; l = l->snext);
   if (l)
      l->zap = 0;               // Not to be deleted
   else
   {
      l = malloc (sizeof (*l));
      if (!l)
         errx (1, "malloc");
      statlink++;
      memset (l, 0, sizeof (*l));
      // Link in to sessions
      l->channel = c;
      l->cprev = &c->sessions;
      l->cnext = c->sessions;
      if (c->sessions)
         c->sessions->cprev = &l->cnext;
      c->sessions = l;
      // Link in to channels
      l->session = s;
      l->sprev = &s->channels;
      l->snext = s->channels;
      if (s->channels)
         s->channels->sprev = &l->snext;
      s->channels = l;
      c->count++;
      if (c->count > c->max)
      {
         c->max++;
         c->ws = realloc (c->ws, sizeof (*c->ws) * c->max);
         if (!c->ws)
            errx (1, "malloc");
         c->ws[c->max - 1] = NULL;
      }
      int q = c->max;
      while (q > 0 && c->ws[q - 1])
         q--;
      if (!q--)
         errx (1, "WTF, no null");
      c->ws[q] = s->ws;
      l->slot = q;
   }
   pthread_mutex_unlock (&sessionmutex);
}

static void
unlink_channel (wasp_chanlink_t * l)
{                               // Already under mutex
   if (debug)
      warnx ("%s Unlink session %s", l->channel->cid, l->session->sid);
   wasp_channel_t *c = l->channel;
   c->ws[l->slot] = NULL;
   // Unlink
   if (l->cnext)
      l->cnext->cprev = l->cprev;
   *l->cprev = l->cnext;
   if (l->snext)
      l->snext->sprev = l->sprev;
   *l->sprev = l->snext;
   // Free
   statlink--;
   free (l);
   // Clean up channel
   if (!--c->count)
   {                            // Dead channel
      if (debug)
         warnx ("%s Channel deleted", c->cid);
      // Unlink
      if (c->next)
         c->next->prev = c->prev;
      *c->prev = c->next;
      statchannel--;
      // Free
      if (c->ws)
         free (c->ws);
      free (c->cid);
      free (c);
   }
}

static char *
wasp_script (wasp_session_t * s, const char *script, xml_t head, size_t len, const unsigned char *data)
{                               // Run script, logs any error, returns malloc'd stdout from script (if any) without newlines
   if (!script || !*script || !s)
      return NULL;              // No script to run
   // Temp files
   char **tmpp = NULL;
   int tmpn = 0;
   int tmp (void)
   {                            // return malloc'd temp file name
      tmpp = realloc (tmpp, ++tmpn * sizeof (*tmpp));
      if (!tmpp)
         errx (1, "malloc");
      tmpp[tmpn - 1] = strdup ("/tmp/wasp.XXXXXX");
      int f = mkstemp (tmpp[tmpn - 1]);
      if (f < 0)
         err (1, "Cannot make temp");
      return f;
   }
   // Environment
   char **envp = NULL;
   int envn = 0;
   char **env (void)
   {
      envp = realloc (envp, ++envn * sizeof (*envp));
      if (!envp)
         errx (1, "malloc");
      envp[envn - 1] = NULL;
      return &envp[envn - 1];
   }
   if (asprintf (env (), "WASPSID=%s", s->sid) < 0)
      errx (1, "malloc");
   if (s->id && asprintf (env (), "WASPID=%s", s->id) < 0)
      errx (1, "malloc");
   if (s->ws)
   {
      unsigned long ping = websocket_ping (s->ws);
      if (ping && asprintf (env (), "PING=%lu.%06lu", ping / 1000000UL, ping % 1000000UL) < 0)
         errx (1, "malloc");
   }
   // Data
   xml_t xml = NULL;
   if (data)
   {
      int o = tmp ();
      if (write (o, data, len) != (ssize_t) len)
         err (1, "Cannot write tmp data");
      close (o);
      if (asprintf (env (), "FILE_MESSAGE=%s", tmpp[tmpn - 1]) < 0)
         errx (1, "malloc");
      xml = xml_tree_parse_json ((char *) data, "json");        // We assume the null
   }
   char *v;
   if (head)
   {                            // headers
      if ((v = xml_get (head, "@IP")) && *v && asprintf (env (), "REMOTE_ADDR=%s", v) < 0)
         errx (1, "malloc");
      xml_t query = xml_find (head, "query");
      if (query)
      {                         // Query data
         if ((v = xml_element_content (query)) && *v && asprintf (env (), "QUERY_STRING=%s", v) < 0)
            errx (1, "malloc");
         xml_attribute_t a = NULL;
         while ((a = xml_attribute_next (query, a)))
         {
            if ((v = xml_attribute_content (a)) && asprintf (env (), "QUERY_%s=%s", xml_attribute_name (a), v) < 0)
               errx (1, "malloc");
            char *p;
            for (p = envp[envn - 1]; *p && *p != '='; p++)
               if (!isalnum (*p))
                  *p = '_';
         }
      }
      char *cookie = NULL;
      xml_t http = xml_find (head, "http");
      if (http)
      {                         // HTTP_ data
         if ((v = xml_element_content (http)) && *v && asprintf (env (), "PATH_INFO=%s", v) < 0)
            errx (1, "malloc");
         xml_attribute_t a = NULL;
         while ((a = xml_attribute_next (http, a)))
         {
            if ((v = xml_attribute_content (a)) && asprintf (env (), "HTTP_%s=%s", xml_attribute_name (a), v) < 0)
               errx (1, "malloc");
            char *p;
            for (p = envp[envn - 1]; *p && *p != '='; p++)
               if (!isalnum (*p))
                  *p = '_';
               else
                  *p = toupper (*p);    // Treat all HTTP as upper case
            if (!strncmp (envp[envn - 1], "HTTP_COOKIE=", 12))
               cookie = envp[envn - 1] + 12;
         }
      }
      if (cookie)
      {
         while (*cookie)
         {
            char *q = cookie;
            while (isspace (*q))
               q++;
            cookie = q;
            while (*q && *q != ';')
               q++;
            if (asprintf (env (), "COOKIE_%.*s", (int) (q - cookie), cookie) < 0)
               errx (1, "malloc");
            char *p;
            for (p = envp[envn - 1]; *p && *p != '='; p++)
               if (!isalnum (*p))
                  *p = '_';
            if (*q)
               q++;             // ;
            cookie = q;
         }
      }
   }
   if (xml)
   {                            // Data passed
      xml_attribute_t a = NULL;
      while ((a = xml_attribute_next (xml, a)))
      {                         // Top level objects (direct, or as file)
         char *n = xml_attribute_name (a);
         char *v = xml_attribute_content (a);
         int l = strlen (v);
         char *p;
         for (p = v; *p && *p != '\n'; p++);
         if (!*p && l < 256 && strncmp (v, "base64;", 7) && strncmp (n, "FILE_", 5))
         {                      // As env
            if (asprintf (env (), "%s=%s", n, v) < 0)
               errx (1, "malloc");
         } else
         {                      // As file
            int f = tmp ();
            if (write (f, v, l) != l)
               err (1, "Cannot write tmp file");
            close (f);
            if (!strncmp (n, "FILE_", 5))
               n += 5;          // Don't do it twice
            if (asprintf (env (), "FILE_%s=%s", n, tmpp[tmpn - 1]) < 0)
               errx (1, "malloc");
         }
      }
      xml_t o = NULL;
      while ((o = xml_element_next (xml, o)))
      {                         // Sub objects as file
         char *n = xml_attribute_name (o);
         char *buf = NULL;
         size_t l = 0;
         FILE *b = open_memstream (&buf, &l);
         xml_write_json (b, o);
         fclose (b);
         if (buf)
         {
            int f = tmp ();
            if (write (f, buf, l) != (ssize_t) l)
               err (1, "Cannot write tmp file");
            close (f);
            if (asprintf (env (), "FILE_%s=%s", n, tmpp[tmpn - 1]) < 0)
               errx (1, "malloc");
            free (buf);
         }
      }
   }
   *env () = NULL;              // End

   int pipefd[2];
   if (pipe (pipefd) < 0)
      err (1, "Bad pipe");
   pid_t child = fork ();
   if (child < 0)
      err (1, "Fork failed");
   if (child)
   {                            // We are parent
      // Some cleanup
      if (xml)
         xml_tree_delete (xml);
      if (envp)
      {                         // Free env
         while (envn--)
            if (envp[envn])
               free (envp[envn]);
         free (envp);
      }
      close (pipefd[1]);        // Write end closed
      int status = 0;
      waitpid (child, &status, 0);
      if (!WIFEXITED (status))
      {
         syslog (LOG_INFO, "Script [%s] failed", script);
         if (debug)
            warnx ("Script [%s] failed", script);

      } else if (WEXITSTATUS (status))
      {
         syslog (LOG_INFO, "Script [%s] exited with %d", script, WEXITSTATUS (status));
         if (debug)
            warnx ("Script [%s] exited with %d", script, WEXITSTATUS (status));
      }
      char *buf = NULL;
      size_t l = 0;
      FILE *b = open_memstream (&buf, &l);
      char temp[1024];
      size_t p;
      while ((p = read (pipefd[0], temp, sizeof (temp))) > 0)
         fwrite (temp, p, 1, b);
      fclose (b);
      if (buf)
         while (l > 0 && buf[l - 1] < ' ')
            buf[--l] = 0;
      close (pipefd[0]);        // Read end closed
      // Temp files delete
      if (tmpp)
      {
         while (tmpn--)
         {
            unlink (tmpp[tmpn]);
            free (tmpp[tmpn]);
         }
         free (tmpp);
      }
      return buf;
   }
   // Child process
   // pipework
   close (pipefd[0]);           // Read end closed
   dup2 (pipefd[1], 1);         // Stdout is pipe
   close (pipefd[1]);           // Write end closed (now copied)
   if (!debug)
   {
      int f = open ("/dev/null", O_WRONLY);
      if (f < 0)
         err (1, "Cannot open /dev/null");
      dup2 (f, 2);              // Stderr
      close (f);
      f = open ("/dev/null", O_RDONLY);
      if (f < 0)
         err (1, "Cannot open /dev/null");
      dup2 (f, 0);              // Stdin
      close (f);
   }
   // Script
   char *type = strrchr (script, '$');
   if (type)
   {                            // variable name as part of script name if possible
      script = strdup (script);
      type = strrchr (script, '$');
      *type++ = 0;
      if (xml)
      {                         // Check for script by type
         xml_attribute_t a = xml_attribute_by_name (xml, type);
         if (a)
         {
            char *v = xml_attribute_content (a);
            if (v && *v)
            {
               v = strdup (v);
               char *p = v;
               while (isalnum (*p) || *p == '-' || *p == '_')
                  p++;
               if (!*p)
               {                // Make script with tag
                  char *f;
                  if (asprintf (&f, "%s%s", script, v) < 0)
                     errx (1, "malloc");
                  if (!access (f, X_OK))
                     script = f;        // Can exec, use it
               }
            }
         }
      }
   }
   if (xml)
      xml_tree_delete (xml);
   const char *tail = strrchr (script, '/');
   if (tail)
      tail++;
   else
      tail = script;
   // Exec
   char **argv = NULL;
   int argn = 0;
   char **arg (void)
   {
      argv = realloc (argv, ++argn * sizeof (*argv));
      if (!argv)
         errx (1, "malloc");
      argv[argn - 1] = NULL;
      return &argv[argn - 1];
   }
   if (asprintf (arg (), "%s", tail) < 0)
      errx (1, "malloc");
   *arg () = NULL;              // End
   if (debug)
   {
      int n;
      fprintf (stderr, "Exec %s", script);
      for (n = 1; n < argn - 1; n++)
         fprintf (stderr, " %s", argv[n]);
      for (n = 0; n < envn - 1; n++)
         fprintf (stderr, "\n%s", envp[n]);
      fprintf (stderr, "\n");
   }
   // Exec
   if (execvpe (script, argv, envp))
      err (1, "Bad exec of [%s]", script);
   // Child failed?
   _exit (1);                   // WTF
   return NULL;
}

// main functions
static void
wasp_connect (wasp_session_t * s, xml_t head)
{
   sessions = s;
   if (debug)
      warnx ("%s connect %s", s->sid, xml_get (head, "@IP"));
   link_channel (s, make_channel (s->sid));     // SID is logically a channel
   // Call connect
   char *r = NULL;
#ifdef	CONNECT
   if (CONNECT (s->sid, head))  // Only run script if return non zero
#endif
      r = wasp_script (s, connectscript, head, 0, NULL);
   if (!r)
   {                            // Failed
      syslog (LOG_INFO, "%s connect failed, disconnecting", s->sid);
      const char *e = websocket_send_raw (1, &s->ws, 0, NULL);  // Sends disconnect
      if (e)
         warnx ("Bad disconnect %s", e);
      return;
   }
   // Add identifier to channel
   if (*r)
   {
      link_channel (s, make_channel (r));
      s->id = r;
      r = NULL;
   }
   if (r)
      free (r);
}

static void
wasp_message (wasp_session_t * s, size_t len, const unsigned char *data)
{
   if (debug)
      warnx ("%s message [%.*s]", s->sid, (int) len, data);
   char *r = NULL;
#ifdef	MESSAGE
   if (MESSAGE (s->sid, len, data))     // Only run script if return non zero
#endif
      r = wasp_script (s, messagescript, NULL, len, data);
   if (r)
      free (r);
}

static void
wasp_disconnect (wasp_session_t * s)
{
   if (debug)
      warnx ("%s disconnect", s->sid);
   char *r = NULL;
#ifdef	DISCONNECT
   if (DISCONNECT (s->sid, s->id))      // Only run script if return non zero
#endif
      r = wasp_script (s, disconnectscript, NULL, 0, NULL);
   if (r)
      free (r);
   pthread_mutex_lock (&sessionmutex);
   statsession--;
   // Unlink from sessions
   if (s->next)
      s->next->prev = s->prev;
   *s->prev = s->next;
   if (s->id)
      free (s->id);
   free (s);
   pthread_mutex_unlock (&sessionmutex);
}

const char *
wasp_command (const char *target, const char *cmd, size_t len, const unsigned char *data)
{                               // Do a command (can be used by connect/disconnect/message)
   int exists = 0;
   if (cmd && *cmd == '*')
   {
      exists = 1;
      cmd++;
   }
   if (!cmd || !*cmd)
      return "Missing command";
   // Find target
   wasp_channel_t *t = find_channel (target);
   if (!t || !t->sessions)
   {                            // Error depends if exists
      if (exists)
         return "Unknown channel";      // Check exists
      if (debug)
         warnx ("%s Command %s, target not found", target, cmd);
      return NULL;              // OK do nothing
   }
   if (debug)
      warnx ("%s Command %s %d/%d", target, cmd, t->count, t->max);
   if (!strcmp (cmd, "disconnect"))
   {
      if (data)
         websocket_send_raw (t->max, t->ws, len, data); // Send message before disconnect (consumes data)
      return websocket_send_raw (t->max, t->ws, 0, NULL);       // Sends disconnects
   }
   if (!strcmp (cmd, "message"))
      return websocket_send_raw (t->max, t->ws, len, data);     // Send message (consumes data)
   if (!strcmp (cmd, "clear") || !strcmp (cmd, "add"))
   {                            // Add
      if (!strcmp (cmd, "clear"))
      {                         // Remove all channels first
         pthread_mutex_lock (&sessionmutex);
         wasp_chanlink_t *l;
         for (l = t->sessions; l; l = l->cnext)
         {                      // Scan the sessions in this channel, for each remove all channels for which they are a member (except sid an id)
            wasp_session_t *s = l->session;
            wasp_chanlink_t *p;
            for (p = s->channels; p; p = p->snext)
               if (strcmp (p->channel->cid, s->id ? : "") && strcmp (p->channel->cid, s->sid))
                  p->zap = 1;
         }
         pthread_mutex_unlock (&sessionmutex);
      }
      if (data && len)
      {
         char *p = (char *) data,
            *e = (char *) data + len;
         while (p < e)
         {
            char *n;
            for (n = p; n < e && *n >= ' '; n++);
            if (n < e)
            {                   // Expects terminator...
               *n++ = 0;
               wasp_channel_t *c = make_channel (p);
               wasp_chanlink_t *l;
               for (l = t->sessions; l; l = l->cnext)
                  link_channel (l->session, c);
            }
            p = n;
            while (p < e && *p < ' ')
               p++;
         }
      }
      if (!strcmp (cmd, "clear"))
      {                         // Remove zapped channels
         pthread_mutex_lock (&sessionmutex);
         wasp_chanlink_t *l = t->sessions;
         while (l)
         {                      // Scan the sessions in this channel, for each remove all channels for which they are a member (except sid an id)
            wasp_chanlink_t *n = l->cnext;
            wasp_session_t *s = l->session;
            wasp_chanlink_t *p = s->channels;
            while (p)
            {
               wasp_chanlink_t *n = p->snext;
               if (p->zap)
                  unlink_channel (p);
               p = n;
            }
            l = n;
         }
         pthread_mutex_unlock (&sessionmutex);
      }
      if (data)
         free ((char *) data);
      return NULL;
   }
   if (!strcmp (cmd, "remove"))
   {                            // Remove specific channels
      if (data && len)
      {
         pthread_mutex_lock (&sessionmutex);
         char *p = (char *) data,
            *e = (char *) data + len;
         while (p < e)
         {
            char *n;
            for (n = p; n < e && *n >= ' '; n++);
            if (n < e)
            {                   // Expects terminator...
               *n++ = 0;
               wasp_channel_t *c = find_channel (p);
               if (c)
               {                // Channel exists, remove from it
                  wasp_chanlink_t *l = t->sessions;
                  while (l)
                  {
                     wasp_chanlink_t *n = l->cnext;
                     wasp_session_t *s = l->session;
                     wasp_chanlink_t *p = s->channels;
                     while (p)
                     {
                        wasp_chanlink_t *n = p->snext;
                        if (p->channel == c)
                           unlink_channel (p);
                        p = n;
                     }
                     l = n;
                  }
               } else if (debug)
                  warnx ("%s not found", p);
            }
            p = n;
            while (p < e && *p < ' ')
               p++;
         }
         pthread_mutex_unlock (&sessionmutex);
      }
      if (data)
         free ((char *) data);
      return NULL;
   }
   if (data)
      free ((char *) data);
   return "Unknown command";
}

static const char *
wasp_web_command (xml_t head, size_t len, const unsigned char *data)
{                               // Run command, return static error, consume data
   // Check localhost
   char *v = xml_get (head, "@IP");
   if (strcmp (v, "127.0.0.1") && strcmp (v, "::1"))
   {
      if (debug)
         warnx ("Access from %s", v);
      return "Invalid";
   }
   // Extract command and target
   const char *cmd = xml_get (head, "http");
   if (cmd && *cmd == '/')
      cmd++;
   const char *target = xml_get (head, "query");
   if (!target || !*target)
   {
      if (data)
         free ((char *) data);
      return "Missing target";
   }
   const char *e = wasp_command (target, cmd, len, data);
   if (data)
      free ((char *) data);
   return e;
}

void *
q_thread (void *p)
{
   p = p;
   while (1)
   {
      struct pollfd p = { qpipe[0], POLLIN, 0 };
      int s = poll (&p, 1, debug ? 1000 : 60000);
      if (s)
      {                         // Something waiting on pipe
         char poke;
         ssize_t l = read (qpipe[0], &poke, sizeof (poke));
         if (l < 0)
            err (1, "q pipe failed");
      }
      if (debug)
      {
         static time_t last = 0;
         time_t now = time (0);
         if (last != now)
         {
            last = now;
            fprintf (stderr, "Session %d Channel %d Link %d\n", statsession, statchannel, statlink);
         }
      }
      volatile q_t *q = NULL;
      if (queue)
      {                         // Get from head of queue
         pthread_mutex_lock (&qmutex);
         if (queue)
         {
            q = queue;
            queue = q->next;
         }
         pthread_mutex_unlock (&qmutex);
      }
      if (!q)
         continue;
      if (q->head && !q->data)
         wasp_connect (q->session, q->head);
      else if (!q->head && q->data)
         wasp_message (q->session, q->len, q->data);
      else if (!q->head && !q->data)
         wasp_disconnect (q->session);
      statqueue--;
      if (q->head)
         xml_tree_delete (q->head);
      if (q->data)
         free ((char *) q->data);
      free ((void *) q);
   }
}

// --------------------------------------------------------------------------------
int
main (int argc, const char *argv[])
{
   int background = 0;
   int dump = 0;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
	 // *INDENT-OFF*
         { "bind", 0, POPT_ARG_STRING | (binding ? POPT_ARGFLAG_SHOW_DEFAULT : 0), &binding, 0, "Port to bind", "[hostname#]port"},
         { "cert", 'c', POPT_ARG_STRING | (certfile ? POPT_ARGFLAG_SHOW_DEFAULT : 0), &certfile, 0, "wss-cert", "filename"},
         { "key", 'k', POPT_ARG_STRING | (keyfile ? POPT_ARGFLAG_SHOW_DEFAULT : 0), &keyfile, 0, "wss-key", "filename"},
         { "connect", 'C', POPT_ARG_STRING | (connectscript ? POPT_ARGFLAG_SHOW_DEFAULT : 0), &connectscript, 0, "connect", "filename"},
         { "message", 'M', POPT_ARG_STRING | (messagescript ? POPT_ARGFLAG_SHOW_DEFAULT : 0), &messagescript, 0, "message", "filename[$field]"},
         { "disconnect", 'D', POPT_ARG_STRING | (disconnectscript ? POPT_ARGFLAG_SHOW_DEFAULT : 0), &disconnectscript, 0, "disconnect", "filename"},
         { "background", 0, POPT_ARG_NONE, &background, 0, "Debug",NULL},
         { "debug", 'v', POPT_ARG_NONE, &debug, 0, "Debug",NULL},
         { "dump", 0, POPT_ARG_NONE, &dump, 0, "WS Dump",NULL},
	 POPT_AUTOHELP { }
	 // *INDENT-ON*
      };
      optCon = poptGetContext (NULL, argc, argv, optionsTable, 0);
      //poptSetOtherOptionHelp (optCon, "");
      int c;
      if ((c = poptGetNextOpt (optCon)) < -1)
         errx (1, "%s: %s\n", poptBadOption (optCon, POPT_BADOPTION_NOALIAS), poptStrerror (c));
      if (poptPeekArg (optCon))
      {
         poptPrintUsage (optCon, stderr, 0);
         return -1;
      }
      poptFreeContext (optCon);
   }
   if (dump)
   {                            // Ensure web socket debug
      extern int websocket_debug;
      websocket_debug = dump;
   }
   openlog ("wasp", LOG_CONS | LOG_PID, LOG_USER);      // Main logging is to syslog
   if (background)
   {                            // Background server (would be better to run from respawn or systemd)
      pid_t child = fork ();
      if (child < 0)
         err (1, "Bad fork");
      if (child)
         return 0;
      if (!debug)
         if (daemon (0, 0))
            errx (1, "daemon");
   }

   {                            // File limits - allow lots of connections at once
      struct rlimit l = {
      };
      if (!getrlimit (RLIMIT_NOFILE, &l))
      {
         syslog (LOG_INFO, "Files %ld/%ld", l.rlim_cur, l.rlim_max);
         l.rlim_cur = l.rlim_max;
         if (setrlimit (RLIMIT_NOFILE, &l))
            syslog (LOG_INFO, "Could not increase files");
      }
      if (!getrlimit (RLIMIT_NPROC, &l))
      {
         syslog (LOG_INFO, "Threads %ld/%ld", l.rlim_cur, l.rlim_max);
         l.rlim_cur = l.rlim_max;
         if (setrlimit (RLIMIT_NPROC, &l))
            syslog (LOG_INFO, "Could not increase threads");
      }
   }

   pthread_mutex_init (&mutex, NULL);   // main mutex
   pthread_mutex_init (&sessionmutex, NULL);    // main mutex
   {                            // Queue thread
      if (pipe2 (qpipe, O_NONBLOCK) < 0)
         errx (1, "pipe2 fail");
      pthread_mutex_init (&qmutex, NULL);       // q mutex
      pthread_t t;
      if (pthread_create (&t, NULL, q_thread, NULL))
         errx (1, "Bad thread start");
      pthread_detach (t);
   }

   char *callback_locked (websocket_t * w, xml_t head, size_t datalen, const unsigned char *data)
   {                            // Single threaded callback from web sockets (wrapped in main mutex)
      if (!w && head)
      {                         // Direct run commands
         const char *e = wasp_web_command (head, datalen, data);
         if (e && debug)
            warnx ("Command failed %s", e);
         if (e)
            syslog (LOG_INFO, "Command failed %s", e);
         if (head)
            xml_tree_delete (head);
         return (char *) e;
      }
      if (!w)
         return "Missing socket";
      // Queue web socket requests (but handle basic connect/disconnect directly for safety)
      wasp_session_t *s = NULL;
      if (w && head && !data)
      {                         // Connect, so allocate socket
         s = malloc (sizeof (*s));
         if (!s)
            errx (1, "Malloc");
         statsession++;
         memset (s, 0, sizeof (*s));
         tokens (s->sid, sizeof (s->sid) - 1);
         s->ws = w;
         websocket_set_data (w, s);
         // Link in to session list
         pthread_mutex_lock (&sessionmutex);
         s->prev = &sessions;
         s->next = sessions;
         if (sessions)
            sessions->prev = &s->next;
         sessions = s;
         pthread_mutex_unlock (&sessionmutex);
      } else
         s = websocket_data (w);
      if (!s)
         return NULL;
      if (s && !head && !data)
      {                         // Disconnect
         pthread_mutex_lock (&sessionmutex);
         // Remove from channels
         while (s->channels)
            unlink_channel (s->channels);
         s->ws = NULL;          // Web socket no longer valid, this is a disconnect
         pthread_mutex_unlock (&sessionmutex);
      }
      pthread_mutex_lock (&qmutex);
      volatile q_t *q = malloc (sizeof (*q));
      if (!q)
         errx (1, "malloc");
      statqueue++;
      memset ((q_t *) q, 0, sizeof (*q));
      q->session = s;
      q->head = head;
      q->len = datalen;
      q->data = data;
      if (queue)
         queueend->next = q;
      else
         queue = q;
      queueend = q;
      char poke = 0;
      if (write (qpipe[1], &poke, sizeof (poke)) < 0)
         errx (1, "queue fail");
      pthread_mutex_unlock (&qmutex);
      return NULL;              // Good
   }

   char *callback (websocket_t * w, xml_t head, size_t datalen, const unsigned char *data)
   {                            // Callback function - do locking around real callback function
      pthread_mutex_lock (&mutex);
      char *e = callback_locked (w, head, datalen, data);
      if (e && *e != '2' && *e != '*')
         syslog (LOG_INFO, "%s", e);
      pthread_mutex_unlock (&mutex);
      return e;
   }

   // Bind web sockets
   const char *e = websocket_bind_raw (binding, NULL, NULL, NULL, certfile, keyfile, callback);
   if (e)
      errx (1, "%s", e);
   // Main task has nothing much to do.
   while (1)
      sleep (1);
   return 0;
}
