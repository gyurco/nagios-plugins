/*****************************************************************************
*
* Nagios check_http plugin
*
* License: GPL
* Copyright (c) 1999-2014 Nagios Plugins Development Team
*
* Description:
*
* This file contains the check_http plugin
*
* This plugin tests the HTTP service on the specified host. It can test
* normal (http) and secure (https) servers, follow redirects, search for
* strings and regular expressions, check connection times, and report on
* certificate expiration times.
*
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*
*****************************************************************************/

/* splint -I. -I../../plugins -I../../lib/ -I/usr/kerberos/include/ ../../plugins/check_http.c */

const char *progname = "check_http";
const char *copyright = "1999-2014";
const char *email = "devel@nagios-plugins.org";

#include "common.h"
#include "netutils.h"
#include "utils.h"
#include "base64.h"
#include <ctype.h>

#define STICKY_NONE 0
#define STICKY_HOST 1
#define STICKY_PORT 2

#define HTTP_EXPECT "HTTP/1."
enum {
    MAX_IPV4_HOSTLENGTH = 255,
    HTTP_PORT = 80,
    HTTPS_PORT = 443,
    MAX_PORT = 65535
};

const static char *proxy_prefix = "PROXY TCP4 0.0.0.0 0.0.0.0 80 80\r\n";

#ifdef HAVE_SSL
int check_cert = FALSE;
int continue_after_check_cert = FALSE;
int ssl_version = 0;
int days_till_exp_warn, days_till_exp_crit;
char *randbuff;
X509 *server_cert;
#  define my_recv(buf, len) ((use_ssl) ? np_net_ssl_read(buf, len) : read(sd, buf, len))
#  define my_send(buf, len) ((use_ssl) ? np_net_ssl_write(buf, len) : send(sd, buf, len, 0))
#else /* ifndef HAVE_SSL */
#  define my_recv(buf, len) read(sd, buf, len)
#  define my_send(buf, len) send(sd, buf, len, 0)
#endif /* HAVE_SSL */
int no_body = FALSE;
int maximum_age = -1;

enum {
    REGS = 2,
    MAX_RE_SIZE = 2048
};
#include "regex.h"
regex_t preg;
regmatch_t pmatch[REGS];
char regexp[MAX_RE_SIZE];
char errbuf[MAX_INPUT_BUFFER];
int cflags = REG_NOSUB | REG_EXTENDED | REG_NEWLINE;
int errcode;
int invert_regex = 0;

struct timeval tv;
struct timeval tv_temp;

#define HTTP_URL "/"
#define CRLF "\r\n"

int specify_port = FALSE;
int server_port = HTTP_PORT;
char server_port_text[6] = "";
char server_type[6] = "http";
char *server_address;
char *host_name;
char *server_url;
char *user_agent;
int server_url_length;
int server_expect_yn = 0;
char server_expect[MAX_INPUT_BUFFER] = HTTP_EXPECT;
char header_expect[MAX_INPUT_BUFFER] = "";
char string_expect[MAX_INPUT_BUFFER] = "";
char output_header_search[30] = "";
char output_string_search[30] = "";
char *warning_thresholds = NULL;
char *critical_thresholds = NULL;
thresholds *thlds;
char user_auth[MAX_INPUT_BUFFER] = "";
char proxy_auth[MAX_INPUT_BUFFER] = "";
int display_html = FALSE;
char **http_opt_headers = NULL;
int http_opt_headers_count = 0;
int have_accept = FALSE;
int onredirect = STATE_OK;
int followsticky = STICKY_NONE;
int use_ssl = FALSE;
int use_sni = FALSE;
int verbose = FALSE;
int show_extended_perfdata = FALSE;
int show_output_body_as_perfdata = FALSE;
int show_url = FALSE;
int proxy_protocol = FALSE;
int sd;
int min_page_len = 0;
int max_page_len = 0;
int redir_depth = 0;
int max_depth = 15;
char *http_method;
char *http_post_data;
char *http_content_type;
char buffer[MAX_INPUT_BUFFER];
char *client_cert = NULL;
char *client_privkey = NULL;

int process_arguments (int, char **);
int check_http (void);
void redir (char *pos, char *status_line);
int server_type_check(const char *type);
int server_port_check(int ssl_flag);
char *perfd_time (double microsec);
char *perfd_time_connect (double microsec);
char *perfd_time_ssl (double microsec);
char *perfd_time_firstbyte (double microsec);
char *perfd_time_headers (double microsec);
char *perfd_time_transfer (double microsec);
char *perfd_size (int page_len);
void print_help (void);
void print_usage (void);

extern int check_hostname;


int
main (int argc, char **argv)
{
    int result = STATE_UNKNOWN;

    setlocale (LC_ALL, "");
    bindtextdomain (PACKAGE, LOCALEDIR);
    textdomain (PACKAGE);

    /* Set default URL. Must be malloced for subsequent realloc if --onredirect=follow */
    server_url = strdup(HTTP_URL);
    server_url_length = strlen(server_url);
    xasprintf (&user_agent, "User-Agent: check_http/v%s (nagios-plugins %s)",
               NP_VERSION, VERSION);

    /* Parse extra opts if any */
    argv=np_extra_opts (&argc, argv, progname);

    if (process_arguments (argc, argv) == ERROR)
        usage4 (_("Could not parse arguments"));

    if (display_html == TRUE)
        printf ("<A HREF=\"%s://%s:%d%s\" target=\"_blank\">",
                use_ssl ? "https" : "http", host_name ? host_name : server_address,
                server_port, server_url);

    /* initialize alarm signal handling, set socket timeout, start timer */
    (void) signal (SIGALRM, socket_timeout_alarm_handler);
    (void) alarm (timeout_interval);
    gettimeofday (&tv, NULL);

    result = check_http ();
    return result;
}

/* Plugin-specific wrapper for vdie() */
void
check_http_die(int state, char* fmt, ...)
{
    char* msg = malloc(4096);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, 4096, fmt, ap );
    va_end(ap);

    if (show_url) {
        die (state, "HTTP %s - %s://%s:%d%s - %s", state_text(state), use_ssl ? "https" : "http", host_name ? host_name : server_address, server_port, server_url, msg);
    }
    else {
        die (state, "HTTP %s - %s", state_text(state), msg);
    }
}

/* check whether a file exists */
void
test_file (char *path)
{
    if (access(path, R_OK) == 0)
        return;
    usage2 (_("file does not exist or is not readable"), path);
}

/* process command-line arguments */
int
process_arguments (int argc, char **argv)
{
    int c = 1;
    char *p;
    char *temp;

    enum {
        INVERT_REGEX = CHAR_MAX + 1,
        SNI_OPTION,
        VERIFY_HOST,
        CONTINUE_AFTER_CHECK_CERT,
        PROXY_PROTOCOL
    };

    int option = 0;
    static struct option longopts[] = {
        STD_LONG_OPTS,
        {"link", no_argument, 0, 'L'},
        {"nohtml", no_argument, 0, 'n'},
        {"ssl", optional_argument, 0, 'S'},
        {"sni", no_argument, 0, SNI_OPTION},
        {"verify-host", no_argument, 0, VERIFY_HOST},
        {"post", required_argument, 0, 'P'},
        {"method", required_argument, 0, 'j'},
        {"IP-address", required_argument, 0, 'I'},
        {"url", required_argument, 0, 'u'},
        {"uri", required_argument, 0, 'u'},
        {"port", required_argument, 0, 'p'},
        {"authorization", required_argument, 0, 'a'},
        {"proxy-authorization", required_argument, 0, 'b'},
        {"proxy", no_argument, 0, PROXY_PROTOCOL},
        {"header-string", required_argument, 0, 'd'},
        {"string", required_argument, 0, 's'},
        {"expect", required_argument, 0, 'e'},
        {"regex", required_argument, 0, 'r'},
        {"ereg", required_argument, 0, 'r'},
        {"eregi", required_argument, 0, 'R'},
        {"linespan", no_argument, 0, 'l'},
        {"onredirect", required_argument, 0, 'f'},
        {"certificate", required_argument, 0, 'C'},
        {"continue-after-certificate", no_argument, 0, CONTINUE_AFTER_CHECK_CERT},
        {"client-cert", required_argument, 0, 'J'},
        {"private-key", required_argument, 0, 'K'},
        {"useragent", required_argument, 0, 'A'},
        {"header", required_argument, 0, 'k'},
        {"no-body", no_argument, 0, 'N'},
        {"max-age", required_argument, 0, 'M'},
        {"content-type", required_argument, 0, 'T'},
        {"pagesize", required_argument, 0, 'm'},
        {"invert-regex", no_argument, NULL, INVERT_REGEX},
        {"use-ipv4", no_argument, 0, '4'},
        {"use-ipv6", no_argument, 0, '6'},
        {"extended-perfdata", no_argument, 0, 'E'},
        {"output-body-as-perfdata", no_argument, 0, 'o'},
        {"show-url", no_argument, 0, 'U'},
        {0, 0, 0, 0}
    };

    if (argc < 2)
        return ERROR;

    for (c = 1; c < argc; c++) {
        if (strcmp ("-to", argv[c]) == 0)
            strcpy (argv[c], "-t");
        if (strcmp ("-hn", argv[c]) == 0)
            strcpy (argv[c], "-H");
        if (strcmp ("-wt", argv[c]) == 0)
            strcpy (argv[c], "-w");
        if (strcmp ("-ct", argv[c]) == 0)
            strcpy (argv[c], "-c");
        if (strcmp ("-nohtml", argv[c]) == 0)
            strcpy (argv[c], "-n");
    }

    while (1) {
        c = getopt_long (argc, argv, "Vvh46t:c:w:A:k:H:P:j:T:I:a:b:d:e:p:s:R:r:u:f:C:J:K:nlLS::m:M:NEoU", longopts, &option);
        if (c == -1 || c == EOF)
            break;

        switch (c) {
        case '?': /* usage */
            usage5 ();
            break;
        case 'h': /* help */
            print_help ();
            exit (STATE_OK);
            break;
        case 'V': /* version */
            print_revision (progname, NP_VERSION);
            exit (STATE_OK);
            break;
        case 't': /* timeout period */
            timeout_interval = parse_timeout_string(optarg);
            break;
        case 'c': /* critical time threshold */
            critical_thresholds = optarg;
            break;
        case 'w': /* warning time threshold */
            warning_thresholds = optarg;
            break;
        case 'A': /* User Agent String */
            xasprintf (&user_agent, "User-Agent: %s", optarg);
            break;
        case 'k': /* Additional headers */
            /*      if (http_opt_headers_count == 0)
                    http_opt_headers = malloc (sizeof (char *) * (++http_opt_headers_count));
                  else */
            http_opt_headers = realloc (http_opt_headers, sizeof(char *) * (++http_opt_headers_count));
            http_opt_headers[http_opt_headers_count - 1] = optarg;
            if (!strncmp(optarg, "Accept:", 7))
                have_accept = TRUE;
            break;
        case 'L': /* show html link */
            display_html = TRUE;
            break;
        case 'n': /* do not show html link */
            display_html = FALSE;
            break;
        case 'C': /* Check SSL cert validity */
#ifdef HAVE_SSL
            if ((temp=strchr(optarg,','))!=NULL) {
                *temp='\0';
                if (!is_intnonneg (optarg))
                    usage2 (_("Invalid certificate expiration period"), optarg);
                days_till_exp_warn = atoi(optarg);
                *temp=',';
                temp++;
                if (!is_intnonneg (temp))
                    usage2 (_("Invalid certificate expiration period"), temp);
                days_till_exp_crit = atoi (temp);
            } else {
                days_till_exp_crit=0;
                if (!is_intnonneg (optarg))
                    usage2 (_("Invalid certificate expiration period"), optarg);
                days_till_exp_warn = atoi (optarg);
            }
            check_cert = TRUE;
            goto enable_ssl;
#endif
        case CONTINUE_AFTER_CHECK_CERT: /* don't stop after the certificate is checked */
#ifdef HAVE_SSL
            continue_after_check_cert = TRUE;
            break;
#endif
        case 'J': /* use client certificate */
#ifdef HAVE_SSL
            test_file(optarg);
            client_cert = optarg;
            goto enable_ssl;
#endif
        case 'K': /* use client private key */
#ifdef HAVE_SSL
            test_file(optarg);
            client_privkey = optarg;
            goto enable_ssl;
#endif
        case 'S': /* use SSL */
#ifdef HAVE_SSL
enable_ssl:
            /* ssl_version initialized to 0 as a default. Only set if it's non-zero.  This helps when we include multiple
               parameters, like -S and -C combinations */
            use_ssl = TRUE;
            if (c=='S' && optarg != NULL) {
                int got_plus = strchr(optarg, '+') != NULL;

                if (!strncmp (optarg, "1.3", 3))
                    ssl_version = got_plus ? MP_TLSv1_3_OR_NEWER : MP_TLSv1_3;
                else if (!strncmp (optarg, "1.2", 3))
                    ssl_version = got_plus ? MP_TLSv1_2_OR_NEWER : MP_TLSv1_2;
                else if (!strncmp (optarg, "1.1", 3))
                    ssl_version = got_plus ? MP_TLSv1_1_OR_NEWER : MP_TLSv1_1;
                else if (optarg[0] == '1')
                    ssl_version = got_plus ? MP_TLSv1_OR_NEWER : MP_TLSv1;
                else if (optarg[0] == '3')
                    ssl_version = got_plus ? MP_SSLv3_OR_NEWER : MP_SSLv3;
                else if (optarg[0] == '2')
                    ssl_version = got_plus ? MP_SSLv2_OR_NEWER : MP_SSLv2;
                else
                    usage4 (_("Invalid option - Valid SSL/TLS versions: 2, 3, 1, 1.1, 1.2, 1.3 (with optional '+' suffix)"));
            }
            if (specify_port == FALSE)
                server_port = HTTPS_PORT;
#else
            /* -C -J and -K fall through to here without SSL */
            usage4 (_("Invalid option - SSL is not available"));
#endif
            break;
        case SNI_OPTION:
            use_sni = TRUE;
            break;
        case VERIFY_HOST:
            check_hostname = 1;
            break;
        case 'f': /* onredirect */
            if (!strcmp (optarg, "stickyport"))
                onredirect = STATE_DEPENDENT, followsticky = STICKY_HOST|STICKY_PORT;
            else if (!strcmp (optarg, "sticky"))
                onredirect = STATE_DEPENDENT, followsticky = STICKY_HOST;
            else if (!strcmp (optarg, "follow"))
                onredirect = STATE_DEPENDENT, followsticky = STICKY_NONE;
            else if (!strcmp (optarg, "unknown"))
                onredirect = STATE_UNKNOWN;
            else if (!strcmp (optarg, "ok"))
                onredirect = STATE_OK;
            else if (!strcmp (optarg, "warning"))
                onredirect = STATE_WARNING;
            else if (!strcmp (optarg, "critical"))
                onredirect = STATE_CRITICAL;
            else usage2 (_("Invalid onredirect option"), optarg);
            if (verbose)
                printf(_("option f:%d \n"), onredirect);
            break;
            /* Note: H, I, and u must be malloc'd or will fail on redirects */
        case 'H': /* Host Name (virtual host) */
            host_name = strdup(optarg);
            if (*host_name == '[') {
                if ((p = strstr (host_name, "]:")) != NULL) /* [IPv6]:port */ {
                    server_port = atoi (p + 2);
                    *++p = '\0'; // Set The host_name sans ":port"
                }
            } else if ((p = strchr (host_name, ':')) != NULL
                       && strchr (++p, ':') == NULL) /* IPv4:port or host:port */ {
                server_port = atoi (p);
                *--p = '\0'; // Set The host_name sans ":port"
            }
            break;
        case 'I': /* Server IP-address */
            server_address = strdup (optarg);
            break;
        case 'u': /* URL path */
            /* server_url is first allocated in main() */
            free(server_url);
            server_url = strdup (optarg);
            server_url_length = strlen (server_url);
            break;
        case 'p': /* Server port */
            if (!is_intnonneg (optarg))
                usage2 (_("Invalid port number"), optarg);
            else {
                server_port = atoi (optarg);
                specify_port = TRUE;
            }
            break;
        case 'a': /* authorization info */
            strncpy (user_auth, optarg, MAX_INPUT_BUFFER - 1);
            user_auth[MAX_INPUT_BUFFER - 1] = 0;
            break;
        case 'b': /* proxy-authorization info */
            strncpy (proxy_auth, optarg, MAX_INPUT_BUFFER - 1);
            proxy_auth[MAX_INPUT_BUFFER - 1] = 0;
            break;
        case 'P': /* HTTP POST data in URL encoded format; ignored if settings already */
            if (! http_post_data)
                http_post_data = strdup (optarg);
            if (! http_method)
                http_method = strdup("POST");
            break;
        case 'j': /* Set HTTP method */
            if (http_method)
                free(http_method);
            http_method = strdup (optarg);
            break;
        case 'd': /* string or substring */
            strncpy (header_expect, optarg, MAX_INPUT_BUFFER - 1);
            header_expect[MAX_INPUT_BUFFER - 1] = 0;
            break;
        case 's': /* string or substring */
            strncpy (string_expect, optarg, MAX_INPUT_BUFFER - 1);
            string_expect[MAX_INPUT_BUFFER - 1] = 0;
            break;
        case 'e': /* string or substring */
            strncpy (server_expect, optarg, MAX_INPUT_BUFFER - 1);
            server_expect[MAX_INPUT_BUFFER - 1] = 0;
            server_expect_yn = 1;
            break;
        case 'T': /* Content-type */
            xasprintf (&http_content_type, "%s", optarg);
            break;
        case 'l': /* linespan */
            cflags &= ~REG_NEWLINE;
            break;
        case 'R': /* regex */
            cflags |= REG_ICASE;
        case 'r': /* regex */
            strncpy (regexp, optarg, MAX_RE_SIZE - 1);
            regexp[MAX_RE_SIZE - 1] = 0;
            errcode = regcomp (&preg, regexp, cflags);
            if (errcode != 0) {
                (void) regerror (errcode, &preg, errbuf, MAX_INPUT_BUFFER);
                printf (_("Could Not Compile Regular Expression: %s"), errbuf);
                return ERROR;
            }
            break;
        case INVERT_REGEX:
            invert_regex = 1;
            break;
        case '4':
            address_family = AF_INET;
            break;
        case '6':

#ifdef USE_IPV6
            address_family = AF_INET6;
#else
            usage4 (_("IPv6 support not available"));
#endif

            break;
        case 'v': /* verbose */
            verbose = TRUE;
            break;
        case 'm': { /* min_page_length */
            char *tmp;
            if (strchr(optarg, ':') != (char *)NULL) {
                /* range, so get two values, min:max */
                tmp = strtok(optarg, ":");
                if (tmp == NULL) {
                    printf("Bad format: try \"-m min:max\"\n");
                    exit (STATE_WARNING);
                } else
                    min_page_len = atoi(tmp);

                tmp = strtok(NULL, ":");
                if (tmp == NULL) {
                    printf("Bad format: try \"-m min:max\"\n");
                    exit (STATE_WARNING);
                } else
                    max_page_len = atoi(tmp);
            } else
                min_page_len = atoi (optarg);
            break;
        }
        case 'N': /* no-body */
            no_body = TRUE;
            break;
        case 'M': { /* max-age */
            int L = strlen(optarg);
            if (L && optarg[L-1] == 'm')
                maximum_age = atoi (optarg) * 60;
            else if (L && optarg[L-1] == 'h')
                maximum_age = atoi (optarg) * 60 * 60;
            else if (L && optarg[L-1] == 'd')
                maximum_age = atoi (optarg) * 60 * 60 * 24;
            else if (L && (optarg[L-1] == 's' ||
                           isdigit (optarg[L-1])))
                maximum_age = atoi (optarg);
            else {
                fprintf (stderr, "unparsable max-age: %s\n", optarg);
                exit (STATE_WARNING);
            }
        }
        break;
        case 'E': /* show extended perfdata */
            show_extended_perfdata = TRUE;
            break;
        case 'o': /* output response body as perfdata */
            show_output_body_as_perfdata = TRUE;
            break;
        case 'U': /* show checked url in output msg */
          show_url = TRUE;
          break;
        case PROXY_PROTOCOL:
          proxy_protocol = TRUE;
          break;
        }
    }

    c = optind;

    if (server_address == NULL && c < argc)
        server_address = strdup (argv[c++]);

    if (host_name == NULL && c < argc)
        host_name = strdup (argv[c++]);

    if (use_sni && host_name == NULL) {
        usage4(_("Server name indication requires that a host name is defined with -H"));
    }

    if (server_address == NULL) {
        if (host_name == NULL)
            usage4 (_("You must specify a server address or host name"));
        else
            server_address = strdup (host_name);
    }

    set_thresholds(&thlds, warning_thresholds, critical_thresholds);

    if (critical_thresholds && thlds->critical->end>(double)timeout_interval)
        timeout_interval = (int)thlds->critical->end + 1;

    if (http_method == NULL)
        http_method = strdup ("GET");

    if (client_cert && !client_privkey)
        usage4 (_("If you use a client certificate you must also specify a private key file"));

    return TRUE;
}



/* Returns 0 if we're still retrieving the headers.
 * Otherwise, returns the length of the header (not including the final newlines)
 */
static int
document_headers_done (const char *full_page)
{
    const char *body;

    for (body = full_page; *body; body++) {
        if (!strncmp (body, "\n\n", 2) || !strncmp (body, "\n\r\n", 3))
            break;
    }

    if (!*body)
        return 0;  /* haven't read end of headers yet */

    return body - full_page;
}

static time_t
parse_time_string (const char *string)
{
    struct tm tm;
    time_t t;
    memset (&tm, 0, sizeof(tm));

    /* Like this: Tue, 25 Dec 2001 02:59:03 GMT */

    if (isupper (string[0])  &&  /* Tue */
            islower (string[1])  &&
            islower (string[2])  &&
            ',' ==   string[3]   &&
            ' ' ==   string[4]   &&
            (isdigit(string[5]) || string[5] == ' ') &&   /* 25 */
            isdigit (string[6])  &&
            ' ' ==   string[7]   &&
            isupper (string[8])  &&  /* Dec */
            islower (string[9])  &&
            islower (string[10]) &&
            ' ' ==   string[11]  &&
            isdigit (string[12]) &&  /* 2001 */
            isdigit (string[13]) &&
            isdigit (string[14]) &&
            isdigit (string[15]) &&
            ' ' ==   string[16]  &&
            isdigit (string[17]) &&  /* 02: */
            isdigit (string[18]) &&
            ':' ==   string[19]  &&
            isdigit (string[20]) &&  /* 59: */
            isdigit (string[21]) &&
            ':' ==   string[22]  &&
            isdigit (string[23]) &&  /* 03 */
            isdigit (string[24]) &&
            ' ' ==   string[25]  &&
            'G' ==   string[26]  &&  /* GMT */
            'M' ==   string[27]  &&  /* GMT */
            'T' ==   string[28]) {

        tm.tm_sec  = 10 * (string[23]-'0') + (string[24]-'0');
        tm.tm_min  = 10 * (string[20]-'0') + (string[21]-'0');
        tm.tm_hour = 10 * (string[17]-'0') + (string[18]-'0');
        tm.tm_mday = 10 * (string[5] == ' ' ? 0 : string[5]-'0') + (string[6]-'0');
        tm.tm_mon = (!strncmp (string+8, "Jan", 3) ? 0 :
                     !strncmp (string+8, "Feb", 3) ? 1 :
                     !strncmp (string+8, "Mar", 3) ? 2 :
                     !strncmp (string+8, "Apr", 3) ? 3 :
                     !strncmp (string+8, "May", 3) ? 4 :
                     !strncmp (string+8, "Jun", 3) ? 5 :
                     !strncmp (string+8, "Jul", 3) ? 6 :
                     !strncmp (string+8, "Aug", 3) ? 7 :
                     !strncmp (string+8, "Sep", 3) ? 8 :
                     !strncmp (string+8, "Oct", 3) ? 9 :
                     !strncmp (string+8, "Nov", 3) ? 10 :
                     !strncmp (string+8, "Dec", 3) ? 11 :
                     -1);
        tm.tm_year = ((1000 * (string[12]-'0') +
                       100 * (string[13]-'0') +
                       10 * (string[14]-'0') +
                       (string[15]-'0'))
                      - 1900);

        tm.tm_isdst = 0;  /* GMT is never in DST, right? */

        if (tm.tm_mon < 0 || tm.tm_mday < 1 || tm.tm_mday > 31)
            return 0;

        /*
        This is actually wrong: we need to subtract the local timezone
        offset from GMT from this value.  But, that's ok in this usage,
        because we only comparing these two GMT dates against each other,
        so it doesn't matter what time zone we parse them in.
        */

        t = mktime (&tm);
        if (t == (time_t) -1) t = 0;

        if (verbose) {
            const char *s = string;
            while (*s && *s != '\r' && *s != '\n')
                fputc (*s++, stdout);
            printf (" ==> %lu\n", (unsigned long) t);
        }

        return t;

    } else {
        return 0;
    }
}

/* Checks if the server 'reply' is one of the expected 'statuscodes' */
static int
expected_statuscode (const char *reply, const char *statuscodes)
{
    char *expected, *code;
    int result = 0;

    if ((expected = strdup (statuscodes)) == NULL)
        die (STATE_UNKNOWN, _("HTTP UNKNOWN - Memory allocation error\n"));

    for (code = strtok (expected, ","); code != NULL; code = strtok (NULL, ","))
        if (strstr (reply, code) != NULL) {
            result = 1;
            break;
        }

    free (expected);
    return result;
}

int chunk_header(char **buf)
{
    int lth = strtol(*buf, buf, 16);

    if (lth <= 0)
        return lth;

    while (**buf !='\0' && **buf != '\r' && **buf != '\n')
        ++*buf;

    // soak up the leading CRLF
    if (**buf && **buf == '\r' && *(++*buf) && **buf == '\n')
      ++*buf;
    else
      die (STATE_UNKNOWN, _("HTTP UNKNOWN - Failed to parse chunked body, invalid format\n"));

    return lth;
}

char *
decode_chunked_page (const char *raw, char *dst)
{
    int  chunksize;
    char *raw_pos = (char*)raw;
    char *dst_pos = (char*)dst;

    for (;;) {
        if ((chunksize = chunk_header(&raw_pos)) == 0)
            break;
        if (chunksize < 0)
            die (STATE_UNKNOWN, _("HTTP UNKNOWN - Failed to parse chunked body, invalid chunk size\n"));

        memmove(dst_pos, raw_pos, chunksize);
        raw_pos += chunksize;
        dst_pos += chunksize;
        *dst_pos = '\0';

        while (*raw_pos && (*raw_pos == '\r' || *raw_pos == '\n'))
            raw_pos++;
    }

    return dst;
}

static char *
header_value (const char *headers, const char *header)
{
    char *s;
    char *value;
    const char *value_end;
    int value_size;

    if (!(s = strcasestr(headers, header))) {
        return NULL;
    }

    s += strlen(header);

    while (*s && (isspace(*s) || *s == ':')) s++;
    while (*s && isspace(*s)) s++;

    value_end = strchr(s, '\r');
    if (!value_end)
        value_end = strchr(s, '\n');
    if (!value_end) {
        // Turns out there's no newline after the header... So it's at the end!
        value_end = s + strlen(s);
    }

    value_size = value_end - s;

    value = malloc(value_size + 1);
    if (!value) {
        die (STATE_UNKNOWN, _("HTTP_UNKNOWN - Memory allocation error\n"));
    }

    if (!strncpy(value, s, value_size)) {
        die(STATE_UNKNOWN, _("HTTP_UNKNOWN - Memory copy failure\n"));
    }
    value[value_size] = '\0';

    return value;
}

static int
chunked_transfer_encoding (const char *headers)
{
    int result;
    char *encoding = header_value(headers, "Transfer-Encoding");
    if (!encoding) {
        return 0;
    }

    if (! strncmp(encoding, "chunked", sizeof("chunked"))) {
        result = 1;
    } else {
        result = 0;
    }

    free(encoding);
    return result;
}

static int
check_document_dates (const char *headers, char **msg)
{
    const char *s;
    char *server_date = 0;
    char *document_date = 0;
    int date_result = STATE_OK;

    s = headers;
    while (*s) {
        const char *field = s;
        const char *value = 0;

        /* Find the end of the header field */
        while (*s && !isspace(*s) && *s != ':')
            s++;

        /* Remember the header value, if any. */
        if (*s == ':')
            value = ++s;

        /* Skip to the end of the header, including continuation lines. */
        while (*s && !(*s == '\n' && (s[1] != ' ' && s[1] != '\t')))
            s++;

        /* Avoid stepping over end-of-string marker */
        if (*s)
            s++;

        /* Process this header. */
        if (value && value > field+2) {
            char *ff = (char *) malloc (value-field);
            char *ss = ff;
            while (field < value-1)
                *ss++ = tolower(*field++);
            *ss++ = 0;

            if (!strcmp (ff, "date") || !strcmp (ff, "last-modified")) {
                const char *e;
                while (*value && isspace (*value))
                    value++;
                for (e = value; *e && *e != '\r' && *e != '\n'; e++)
                    ;
                ss = (char *) malloc (e - value + 1);
                strncpy (ss, value, e - value);
                ss[e - value] = 0;
                if (!strcmp (ff, "date")) {
                    if (server_date) free (server_date);
                    server_date = ss;
                } else {
                    if (document_date) free (document_date);
                    document_date = ss;
                }
            }
            free (ff);
        }
    }

    /* Done parsing the body.  Now check the dates we (hopefully) parsed.  */
    if (!server_date || !*server_date) {
        xasprintf (msg, _("%sServer date unknown, "), *msg);
        date_result = max_state_alt(STATE_UNKNOWN, date_result);
    } else if (!document_date || !*document_date) {
        xasprintf (msg, _("%sDocument modification date unknown, "), *msg);
        date_result = max_state_alt(STATE_CRITICAL, date_result);
    } else {
        time_t srv_data = parse_time_string (server_date);
        time_t doc_data = parse_time_string (document_date);

        if (srv_data <= 0) {
            xasprintf (msg, _("%sServer date \"%100s\" unparsable, "), *msg, server_date);
            date_result = max_state_alt(STATE_CRITICAL, date_result);
        } else if (doc_data <= 0) {
            xasprintf (msg, _("%sDocument date \"%100s\" unparsable, "), *msg, document_date);
            date_result = max_state_alt(STATE_CRITICAL, date_result);
        } else if (doc_data > srv_data + 30) {
            xasprintf (msg, _("%sDocument is %d seconds in the future, "), *msg, (int)doc_data - (int)srv_data);
            date_result = max_state_alt(STATE_CRITICAL, date_result);
        } else if (doc_data < srv_data - maximum_age) {
            int n = (srv_data - doc_data);
            if (n > (60 * 60 * 24 * 2)) {
                xasprintf (msg, _("%sLast modified %.1f days ago, "), *msg, ((float) n) / (60 * 60 * 24));
                date_result = max_state_alt(STATE_CRITICAL, date_result);
            } else {
                xasprintf (msg, _("%sLast modified %d:%02d:%02d ago, "), *msg, n / (60 * 60), (n / 60) % 60, n % 60);
                date_result = max_state_alt(STATE_CRITICAL, date_result);
            }
        }
        free (server_date);
        free (document_date);
    }
    return date_result;
}

int
get_content_length (const char *headers)
{
    const char *s;
    int content_length = -1;

    s = headers;
    while (*s) {
        const char *field = s;
        const char *value = 0;

        /* Find the end of the header field */
        while (*s && !isspace(*s) && *s != ':')
            s++;

        /* Remember the header value, if any. */
        if (*s == ':')
            value = ++s;

        /* Skip to the end of the header, including continuation lines. */
        while (*s && !(*s == '\n' && (s[1] != ' ' && s[1] != '\t')))
            s++;

        /* Avoid stepping over end-of-string marker */
        if (*s)
            s++;

        /* Process this header. */
        if (value && value > field+2) {
            char *ff = (char *) malloc (value-field);
            char *ss = ff;
            while (field < value-1)
                *ss++ = tolower(*field++);
            *ss++ = 0;

            if (!strcmp (ff, "content-length")) {
                const char *e;
                while (*value && isspace (*value))
                    value++;
                for (e = value; *e && *e != '\r' && *e != '\n'; e++)
                    ;
                ss = (char *) malloc (e - value + 1);
                strncpy (ss, value, e - value);
                ss[e - value] = 0;
                content_length = atoi(ss);
                free (ss);
            }
            free (ff);
        }
    }
    return (content_length);
}

char *
prepend_slash (char *path)
{
    char *newpath;

    if (path[0] == '/')
        return path;

    if ((newpath = malloc (strlen(path) + 2)) == NULL)
        die (STATE_UNKNOWN, _("HTTP UNKNOWN - Memory allocation error\n"));
    newpath[0] = '/';
    strcpy (newpath + 1, path);
    free (path);
    return newpath;
}

int
check_http (void)
{
    char *msg;
    char *status_line;
    char *status_code;
    char *header;
    char *page;
    char *auth;
    int http_status;
    int header_end;
    int content_length;
    int content_start;
    int seen_length;
    int i = 0;
    size_t pagesize = 0;
    char *full_page;
    char *full_page_new;
    char *buf;
    char *pos;
    long microsec = 0L;
    double elapsed_time = 0.0;
    long microsec_connect = 0L;
    double elapsed_time_connect = 0.0;
    long microsec_ssl = 0L;
    double elapsed_time_ssl = 0.0;
    long microsec_firstbyte = 0L;
    double elapsed_time_firstbyte = 0.0;
    long microsec_headers = 0L;
    double elapsed_time_headers = 0.0;
    long microsec_transfer = 0L;
    double elapsed_time_transfer = 0.0;
    int page_len = 0;
    int result = STATE_OK;
    char *force_host_header = NULL;
    int bad_response = FALSE;
    char save_char;

    /* try to connect to the host at the given port number */
    gettimeofday (&tv_temp, NULL);
    if (my_tcp_connect (server_address, server_port, &sd) != STATE_OK)
        die (STATE_CRITICAL, _("HTTP CRITICAL - Unable to open TCP socket\n"));
    microsec_connect = deltime (tv_temp);

    /* Prepend PROXY protocol v1 header if requested */
    if (proxy_protocol == TRUE) {
        if (verbose)
            printf ("Sending header %s\n", proxy_prefix);

        send(sd, proxy_prefix, strlen(proxy_prefix), 0);
    }

    /* if we are called with the -I option, the -j method is CONNECT and */
    /* we received -S for SSL, then we tunnel the request through a proxy*/
    /* @20100414, public[at]frank4dd.com, http://www.frank4dd.com/howto  */

    if ( server_address != NULL && strcmp(http_method, "CONNECT") == 0
            && host_name != NULL && use_ssl == TRUE) {

        if (verbose) printf ("Entering CONNECT tunnel mode with proxy %s:%d to dst %s:%d\n", server_address, server_port, host_name, HTTPS_PORT);
        asprintf (&buf, "%s %s:%d HTTP/1.1\r\n%s\r\n", http_method, host_name, HTTPS_PORT, user_agent);
        asprintf (&buf, "%sProxy-Connection: keep-alive\r\n", buf);
        asprintf (&buf, "%sHost: %s\r\n", buf, host_name);
        /* we finished our request, send empty line with CRLF */
        asprintf (&buf, "%s%s", buf, CRLF);
        if (verbose) printf ("%s\n", buf);
        send(sd, buf, strlen (buf), 0);
        buf[0]='\0';

        if (verbose) printf ("Receive response from proxy\n");
        read (sd, buffer, MAX_INPUT_BUFFER-1);
        if (verbose) printf ("%s", buffer);
        /* Here we should check if we got HTTP/1.1 200 Connection established */
    }
#ifdef HAVE_SSL
    elapsed_time_connect = (double)microsec_connect / 1.0e6;
    if (use_ssl == TRUE) {
        gettimeofday (&tv_temp, NULL);
        result = np_net_ssl_init_with_hostname_version_and_cert(sd, (use_sni ? host_name : NULL), ssl_version, client_cert, client_privkey);
        if (verbose) printf ("SSL initialized\n");
        if (result != STATE_OK)
            die (STATE_CRITICAL, NULL);
        microsec_ssl = deltime (tv_temp);
        elapsed_time_ssl = (double)microsec_ssl / 1.0e6;
        if (check_cert == TRUE) {
            result = np_net_ssl_check_cert(days_till_exp_warn, days_till_exp_crit);
            if (continue_after_check_cert == FALSE) {

                if (sd) {
                    close(sd);
                }
                np_net_ssl_cleanup();
                return result;
            }
        }
    }
#endif /* HAVE_SSL */

    if ( server_address != NULL && strcmp(http_method, "CONNECT") == 0
            && host_name != NULL && use_ssl == TRUE)
        asprintf (&buf, "%s %s %s\r\n%s\r\n", "GET", server_url, host_name ? "HTTP/1.1" : "HTTP/1.0", user_agent);
    else
        asprintf (&buf, "%s %s %s\r\n%s\r\n", http_method, server_url, host_name ? "HTTP/1.1" : "HTTP/1.0", user_agent);

    /* tell HTTP/1.1 servers not to keep the connection alive */
    xasprintf (&buf, "%sConnection: close\r\n", buf);

    /* check if Host header is explicitly set in options */
    if (http_opt_headers_count) {
        for (i = 0; i < http_opt_headers_count ; i++) {
            if (strncmp(http_opt_headers[i], "Host:", 5) == 0) {
                force_host_header = http_opt_headers[i];
            }
        }
    }

    /* optionally send the host header info */
    if (host_name) {
        if (force_host_header) {
            xasprintf (&buf, "%s%s\r\n", buf, force_host_header);
        } else {
            /*
             * Specify the port only if we're using a non-default port (see RFC 2616,
             * 14.23).  Some server applications/configurations cause trouble if the
             * (default) port is explicitly specified in the "Host:" header line.
             */
            if ((use_ssl == FALSE && server_port == HTTP_PORT) ||
                    (use_ssl == TRUE && server_port == HTTPS_PORT))
                xasprintf (&buf, "%sHost: %s\r\n", buf, host_name);
            else
                xasprintf (&buf, "%sHost: %s:%d\r\n", buf, host_name, server_port);
        }
    }

    /* Inform server we accept any MIME type response
     * TODO: Take an argument to determine what type(s) to accept,
     * so that we can alert if a response is of an invalid type.
    */
    if (!have_accept)
        xasprintf(&buf, "%sAccept: */*\r\n", buf);

    /* optionally send any other header tag */
    if (http_opt_headers_count) {
        for (i = 0; i < http_opt_headers_count ; i++) {
            if (force_host_header != http_opt_headers[i]) {
                xasprintf (&buf, "%s%s\r\n", buf, http_opt_headers[i]);
            }
        }
        /* This cannot be free'd here because a redirection will then try to access this and segfault */
        /* Covered in a testcase in tests/check_http.t */
        /* free(http_opt_headers); */
    }

    /* optionally send the authentication info */
    if (strlen(user_auth)) {
        base64_encode_alloc (user_auth, strlen (user_auth), &auth);
        xasprintf (&buf, "%sAuthorization: Basic %s\r\n", buf, auth);
    }

    /* optionally send the proxy authentication info */
    if (strlen(proxy_auth)) {
        base64_encode_alloc (proxy_auth, strlen (proxy_auth), &auth);
        xasprintf (&buf, "%sProxy-Authorization: Basic %s\r\n", buf, auth);
    }

    /* either send http POST data (any data, not only POST)*/
    if (http_post_data) {
        if (http_content_type) {
            xasprintf (&buf, "%sContent-Type: %s\r\n", buf, http_content_type);
        } else {
            xasprintf (&buf, "%sContent-Type: application/x-www-form-urlencoded\r\n", buf);
        }

        xasprintf (&buf, "%sContent-Length: %i\r\n\r\n", buf, (int)strlen (http_post_data));
        xasprintf (&buf, "%s%s%s", buf, http_post_data, CRLF);
    } else {
        /* or just a newline so the server knows we're done with the request */
        xasprintf (&buf, "%s%s", buf, CRLF);
    }

    if (verbose) printf ("%s\n", buf);
    gettimeofday (&tv_temp, NULL);
    my_send (buf, strlen (buf));
    microsec_headers = deltime (tv_temp);
    elapsed_time_headers = (double)microsec_headers / 1.0e6;

    /* fetch the page */
    full_page = strdup("");
    gettimeofday (&tv_temp, NULL);
    while ((i = my_recv (buffer, MAX_INPUT_BUFFER-1)) > 0) {
        if ((i >= 1) && (elapsed_time_firstbyte <= 0.000001)) {
            microsec_firstbyte = deltime (tv_temp);
            elapsed_time_firstbyte = (double)microsec_firstbyte / 1.0e6;
        }
        buffer[i] = '\0';
        /* xasprintf (&full_page_new, "%s%s", full_page, buffer); */
        if ((full_page_new = realloc(full_page, pagesize + i + 1)) == NULL)
            die (STATE_UNKNOWN, _("HTTP UNKNOWN - Could not allocate memory for full_page\n"));

        memmove(&full_page_new[pagesize], buffer, i + 1);
        /*free (full_page);*/
        full_page = full_page_new;
        pagesize += i;

        header_end = document_headers_done(full_page);
        if (header_end) {
            i = 0;
            break;
        }
    }

    if (no_body) {
        full_page[header_end] = '\0';
    }
    else {
        content_length = get_content_length(full_page);

        content_start = header_end + 1;
        while (full_page[content_start] == '\n' || full_page[content_start] == '\r') {
            content_start += 1;
        }
        seen_length = pagesize - content_start;
        /* Continue receiving the body until content-length is met */
        while ((content_length < 0 || seen_length < content_length)
            && ((i = my_recv(buffer, MAX_INPUT_BUFFER-1)) > 0)) {

            buffer[i] = '\0';

            if ((full_page_new = realloc(full_page, pagesize + i + 1)) == NULL)
                die (STATE_UNKNOWN, _("HTTP UNKNOWN - Could not allocate memory for full_page\n"));
            memmove(&full_page_new[pagesize], buffer, i + 1);
            full_page = full_page_new;

            pagesize += i;
            seen_length = pagesize - content_start;
        }
    }

    microsec_transfer = deltime (tv_temp);
    elapsed_time_transfer = (double)microsec_transfer / 1.0e6;

    if (i < 0 && errno != ECONNRESET) {
#ifdef HAVE_SSL
        /*
        if (use_ssl) {
          sslerr=SSL_get_error(ssl, i);
          if ( sslerr == SSL_ERROR_SSL ) {
            die (STATE_WARNING, _("HTTP WARNING - Client Certificate Required\n"));
          } else {
            die (STATE_CRITICAL, _("HTTP CRITICAL - Error on receive\n"));
          }
        }
        else {
        */
#endif
        die (STATE_CRITICAL, _("HTTP CRITICAL - Error on receive\n"));
#ifdef HAVE_SSL
        /* XXX
        }
        */
#endif
    }

    /* return a CRITICAL status if we couldn't read any data */
    if (pagesize == (size_t) 0)
        die (STATE_CRITICAL, _("HTTP CRITICAL - No data received from host\n"));

    /* close the connection */
    if (sd) close(sd);
#ifdef HAVE_SSL
    np_net_ssl_cleanup();
#endif

    /* Save check time */
    microsec = deltime (tv);
    elapsed_time = (double)microsec / 1.0e6;

    /* leave full_page untouched so we can free it later */
    pos = page = full_page;

    if (verbose)
        printf ("%s://%s:%d%s is %d characters\n",
            use_ssl ? "https" : "http", server_address,
            server_port, server_url, (int)pagesize);

    /* find status line and null-terminate it */
    page += (size_t) strcspn (page, "\r\n");
    save_char = *page;
    *page = '\0';
    status_line = strdup(pos);
    *page = save_char;
    pos = page;


    strip (status_line);
    if (verbose)
        printf ("STATUS: %s\n", status_line);

    /* find header info and null-terminate it */
    header = page;

    for (;;) {

        if ((page == NULL) || !strncmp(page, "\r\n\r\n", 4) || !strncmp(page, "\n\n", 2) || *page == '\0' )
            break;

        while (*page == '\r' || *page == '\n') {
            ++page;
        }

        page += (size_t) strcspn (page, "\r\n");
        pos = page;

        if (*page == '\0')
            break;
    }

    page += (size_t) strspn (page, "\r\n");
    header[pos - header] = 0;
    while (*header == '\r' || *header == '\n') {
        ++header;
    }

    if (chunked_transfer_encoding(header) && *page)
        page = decode_chunked_page(page, page);

    if (verbose)
        printf ("**** HEADER ****\n%s\n**** CONTENT ****\n%s\n", header,
                (no_body ? "  [[ skipped ]]" : page));

    xasprintf(&msg, "");

    /* make sure the status line matches the response we are looking for */
    if (!expected_statuscode (status_line, server_expect)) {

        if (server_port == HTTP_PORT)
            xasprintf (&msg,
                       _("Invalid HTTP response received from host: %s\n"),
                       status_line);
        else
            xasprintf (&msg,
                       _("Invalid HTTP response received from host on port %d: %s\n"),
                       server_port, status_line);
        bad_response = TRUE;
    }

    /* Bypass normal status line check if server_expect was set by user and not default */
    if ( server_expect_yn && !bad_response )  {

        xasprintf (&msg,
                   _("Status line output matched \"%s\" - "), server_expect);

        if (verbose)
            printf ("%s\n",msg);

    } else {

        /* Status-Line = HTTP-Version SP Status-Code SP Reason-Phrase CRLF */
        /* HTTP-Version   = "HTTP" "/" 1*DIGIT "." 1*DIGIT */
        /* Status-Code = 3 DIGITS */

        if (status_line != NULL) {

            status_code = strchr(status_line, ' ');
            if (status_code != NULL) 
                /* Normally the following line runs once, but some servers put extra whitespace between the version number and status code. */
                while (*status_code == ' ') { status_code += sizeof(char); }

            if (status_code == NULL || (strspn(status_code, "1234567890") != 3))
                check_http_die (STATE_CRITICAL, _("Invalid Status Line (%s)\n"), status_line);

        } else {

            check_http_die (STATE_CRITICAL, _("No Status Line\n"));
        }

        http_status = atoi (status_code);

        /* check the return code */

        if (http_status >= 600 || http_status < 100) {
            check_http_die (STATE_CRITICAL, _("Invalid Status (%s)\n"), status_line);
        }

        /* server errors result in a critical state */
        else if (http_status >= 500) {
            xasprintf (&msg, _("%s%s - "), msg, status_line);
            if (bad_response || !server_expect_yn)
                result = STATE_CRITICAL;
        }

        /* client errors result in a warning state */
        else if (http_status >= 400) {
            xasprintf (&msg, _("%s%s - "), msg, status_line);
            if (bad_response || !server_expect_yn)
                result = max_state_alt(STATE_WARNING, result);
        }

        /* check redirected page if specified */
        else if (http_status >= 300) {

            if (onredirect == STATE_DEPENDENT)
                redir (header, status_line);
            else
                result = max_state_alt(onredirect, result);
            xasprintf (&msg, _("%s%s - "), msg, status_line);
        } 

        /* end if (http_status >= 300) */
        else if (!bad_response) {

            /* Print OK status anyway */
            xasprintf (&msg, _("%s%s - "), msg, status_line);
        }

    } /* end else [if (server_expect_yn)] */

    free(status_line);

    if (bad_response)
        check_http_die (STATE_CRITICAL, msg);

    /* reset the alarm - must be called *after* redir or we'll never die on redirects! */
    alarm (0);

    if (maximum_age >= 0) {
        result = max_state_alt(check_document_dates(header, &msg), result);
    }


    /* Page and Header content checks go here */
    if (strlen (header_expect)) {
        if (!strstr (header, header_expect)) {
            strncpy(&output_header_search[0],header_expect,sizeof(output_header_search));
            if(output_header_search[sizeof(output_header_search)-1]!='\0') {
                bcopy("...",&output_header_search[sizeof(output_header_search)-4],4);
            }
            xasprintf (&msg, _("%sheader '%s' not found on '%s://%s:%d%s', "), msg, output_header_search, use_ssl ? "https" : "http", host_name ? host_name : server_address, server_port, server_url);
            result = STATE_CRITICAL;
        }
    }

    if (strlen (string_expect)) {
        if (!strstr (page, string_expect)) {
            strncpy(&output_string_search[0],string_expect,sizeof(output_string_search));
            if(output_string_search[sizeof(output_string_search)-1]!='\0') {
                bcopy("...",&output_string_search[sizeof(output_string_search)-4],4);
            }
            xasprintf (&msg, _("%sstring '%s' not found on '%s://%s:%d%s', "), msg, output_string_search, use_ssl ? "https" : "http", host_name ? host_name : server_address, server_port, server_url);
            result = STATE_CRITICAL;
        }
    }

    if (strlen (regexp)) {
        errcode = regexec (&preg, page, REGS, pmatch, 0);
        if ((errcode == 0 && invert_regex == 0) || (errcode == REG_NOMATCH && invert_regex == 1)) {
            /* OK - No-op to avoid changing the logic around it */
            result = max_state_alt(STATE_OK, result);
        } else if ((errcode == REG_NOMATCH && invert_regex == 0) || (errcode == 0 && invert_regex == 1)) {
            if (invert_regex == 0)
                xasprintf (&msg, _("%spattern not found, "), msg);
            else
                xasprintf (&msg, _("%spattern found, "), msg);
            result = STATE_CRITICAL;
        } else {
            /* FIXME: Shouldn't that be UNKNOWN? */
            regerror (errcode, &preg, errbuf, MAX_INPUT_BUFFER);
            xasprintf (&msg, _("%sExecute Error: %s, "), msg, errbuf);
            result = STATE_CRITICAL;
        }
    }

    /* make sure the page is of an appropriate size */
    /* page_len = get_content_length(header); */
    /* FIXME: Will this work with -N ? IMHO we should use
     * get_content_length(header) and always check if it's different than the
     * returned pagesize
     */
    /* FIXME: IIRC pagesize returns headers - shouldn't we make
     * it == get_content_length(header) ??
     */
    page_len = pagesize;
    if ((max_page_len > 0) && (page_len > max_page_len)) {
        xasprintf (&msg, _("%spage size %d too large, "), msg, page_len);
        result = max_state_alt(STATE_WARNING, result);
    } else if ((min_page_len > 0) && (page_len < min_page_len)) {
        xasprintf (&msg, _("%spage size %d too small, "), msg, page_len);
        result = max_state_alt(STATE_WARNING, result);
    }

    /* Cut-off trailing characters */
    if(msg[strlen(msg)-2] == ',') {
        msg[strlen(msg)-2] = '\0';
    }
    else {
        msg[strlen(msg)-3] = '\0';
    }

    /* show checked URL */
    if (show_url)
        xasprintf (&msg, _("%s - %s://%s:%d%s"), msg, use_ssl ? "https" : "http", host_name ? host_name : server_address, server_port, server_url);



    /* check elapsed time */
    if (show_extended_perfdata) {
        xasprintf (&msg,
                   _("%s - %d bytes in %.3f second response time %s|%s %s %s %s %s %s %s %s"),
                   msg, page_len, elapsed_time,
                   (display_html ? "</A>" : ""),
                   perfd_time (elapsed_time),
                   perfd_size (page_len),
                   perfd_time_connect (elapsed_time_connect),
                   use_ssl == TRUE ? perfd_time_ssl (elapsed_time_ssl) : "",
                   perfd_time_headers (elapsed_time_headers),
                   perfd_time_firstbyte (elapsed_time_firstbyte),
                   perfd_time_transfer (elapsed_time_transfer),
                   (result == STATE_OK && show_output_body_as_perfdata ? page : ""));
    }
    else {
        xasprintf (&msg,
                   _("%s - %d bytes in %.3f second response time %s|%s %s %s"),
                   msg, page_len, elapsed_time,
                   (display_html ? "</A>" : ""),
                   perfd_time (elapsed_time),
                   perfd_size (page_len),
                   (result == STATE_OK && show_output_body_as_perfdata ? page : ""));
    }

    result = max_state_alt(get_status(elapsed_time, thlds), result);

    die (result, "HTTP %s: %s\n", state_text(result), msg);

    /* die failed? */
    return STATE_UNKNOWN;
}



/* per RFC 2396 */
#define URI_HTTP "%5[HTPShtps]"
#define URI_HOST "%255[-.abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789]"
#define URI_PORT "%6d" /* MAX_PORT's width is 5 chars, 6 to detect overflow */
#define URI_PATH "%[-_.!~*'();/?:@&=+$,%#abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789]"
#define HD1 URI_HTTP "://" URI_HOST ":" URI_PORT "/" URI_PATH
#define HD2 URI_HTTP "://" URI_HOST "/" URI_PATH
#define HD3 URI_HTTP "://" URI_HOST ":" URI_PORT
#define HD4 URI_HTTP "://" URI_HOST
/* HD5 - relative reference redirect like //www.site.org/test https://tools.ietf.org/html/rfc3986 */
#define HD5 URI_HTTP "//" URI_HOST "/" URI_PATH
#define HD6 URI_PATH

void
redir (char *pos, char *status_line)
{
    int i = 0;
    char *x;
    char xx[2];
    char type[6];
    char *addr;
    char *url;

    addr = malloc (MAX_IPV4_HOSTLENGTH + 1);
    if (addr == NULL)
        die (STATE_UNKNOWN, _("HTTP UNKNOWN - Could not allocate addr\n"));

    memset(addr, 0, MAX_IPV4_HOSTLENGTH);
    url = malloc (strcspn (pos, "\r\n"));
    if (url == NULL)
        die (STATE_UNKNOWN, _("HTTP UNKNOWN - Could not allocate URL\n"));

    while (pos) {
        sscanf (pos, "%1[Ll]%*1[Oo]%*1[Cc]%*1[Aa]%*1[Tt]%*1[Ii]%*1[Oo]%*1[Nn]:%n", xx, &i);
        if (i == 0) {
            pos += (size_t) strcspn (pos, "\r\n");
            pos += (size_t) strspn (pos, "\r\n");
            if (strlen(pos) == 0)
                die (STATE_UNKNOWN,
                     _("HTTP UNKNOWN - Could not find redirect location - %s%s\n"),
                     status_line, (display_html ? "</A>" : ""));
            continue;
        }

        pos += i;
        pos += strspn (pos, " \t");

        /*
         * RFC 2616 (4.2):  ``Header fields can be extended over multiple lines by
         * preceding each extra line with at least one SP or HT.''
         */
        for (; (i = strspn (pos, "\r\n")); pos += i) {
            pos += i;
            if (!(i = strspn (pos, " \t"))) {
                die (STATE_UNKNOWN, _("HTTP UNKNOWN - Empty redirect location%s\n"),
                     display_html ? "</A>" : "");
            }
        }

        url = realloc (url, strcspn (pos, "\r\n") + 1);
        if (url == NULL)
            die (STATE_UNKNOWN, _("HTTP UNKNOWN - Could not allocate URL\n"));

        /* URI_HTTP, URI_HOST, URI_PORT, URI_PATH */
        if (sscanf (pos, HD1, type, addr, &i, url) == 4) {
            url = prepend_slash (url);
            use_ssl = server_type_check (type);
        }

        /* URI_HTTP URI_HOST URI_PATH */
        else if (sscanf (pos, HD2, type, addr, url) == 3 ) {
            url = prepend_slash (url);
            use_ssl = server_type_check (type);
            i = server_port_check (use_ssl);
        }

        /* URI_HTTP URI_HOST URI_PORT */
        else if (sscanf (pos, HD3, type, addr, &i) == 3) {
            strcpy (url, HTTP_URL);
            use_ssl = server_type_check (type);
        }

        /* URI_HTTP URI_HOST */
        else if (sscanf (pos, HD4, type, addr) == 2) {
            strcpy (url, HTTP_URL);
            use_ssl = server_type_check (type);
            i = server_port_check (use_ssl);
        }

        /* URI_HTTP, URI_HOST, URI_PATH */
        else if (sscanf (pos, HD5, addr, url) == 2) {
            if(use_ssl)
                strcpy (type,"https");
            else
                strcpy (type,server_type);
            xasprintf(&url, "/%s", url);
            use_ssl = server_type_check (type);
            i = server_port_check (use_ssl);
        }

        /* URI_PATH */
        else if (sscanf (pos, HD6, url) == 1) {
            /* relative url */
            if ((url[0] != '/')) {
                if ((x = strrchr(server_url, '/')))
                    *x = '\0';
                xasprintf (&url, "%s/%s", server_url, url);
            }
            i = server_port;
            strcpy (type, server_type);
            strcpy (addr, host_name ? host_name : server_address);
        }

        else {
            die (STATE_UNKNOWN,
                 _("HTTP UNKNOWN - Could not parse redirect location - %s%s\n"),
                 pos, (display_html ? "</A>" : ""));
        }

        break;

    } /* end while (pos) */

    if (++redir_depth > max_depth)
        die (STATE_WARNING,
             _("HTTP WARNING - maximum redirection depth %d exceeded - %s://%s:%d%s%s\n"),
             max_depth, type, addr, i, url, (display_html ? "</A>" : ""));

    if (server_port==i &&
            !strncmp(server_address, addr, MAX_IPV4_HOSTLENGTH) &&
            (host_name && !strncmp(host_name, addr, MAX_IPV4_HOSTLENGTH)) &&
            !strcmp(server_url, url))
        die (STATE_WARNING,
             _("HTTP WARNING - redirection creates an infinite loop - %s://%s:%d%s%s\n"),
             type, addr, i, url, (display_html ? "</A>" : ""));

    strcpy (server_type, type);

    free (host_name);
    host_name = strndup (addr, MAX_IPV4_HOSTLENGTH);

    if (!(followsticky & STICKY_HOST)) {
        free (server_address);
        server_address = strndup (addr, MAX_IPV4_HOSTLENGTH);
    }
    if (!(followsticky & STICKY_PORT)) {
        server_port = i;
    }

    free (server_url);
    server_url = url;

    if (server_port > MAX_PORT)
        die (STATE_UNKNOWN,
             _("HTTP UNKNOWN - Redirection to port above %d - %s://%s:%d%s%s\n"),
             MAX_PORT, server_type, server_address, server_port, server_url,
             display_html ? "</A>" : "");

    if (verbose)
        printf (_("Redirection to %s://%s:%d%s\n"), server_type,
                host_name ? host_name : server_address, server_port, server_url);

    free(addr);
    check_http ();
}


int
server_type_check (const char *type)
{
    if (strcmp (type, "https"))
        return FALSE;
    else
        return TRUE;
}

int
server_port_check (int ssl_flag)
{
    if (ssl_flag)
        return HTTPS_PORT;
    else
        return HTTP_PORT;
}

char *perfd_time (double elapsed_time)
{
    return fperfdata ("time", elapsed_time, "s",
                      thlds->warning?TRUE:FALSE, thlds->warning?thlds->warning->end:0,
                      thlds->critical?TRUE:FALSE, thlds->critical?thlds->critical->end:0,
                      TRUE, 0, FALSE, 0);
}

char *perfd_time_connect (double elapsed_time_connect)
{
    return fperfdata ("time_connect", elapsed_time_connect, "s", FALSE, 0, FALSE, 0, FALSE, 0, FALSE, 0);
}

char *perfd_time_ssl (double elapsed_time_ssl)
{
    return fperfdata ("time_ssl", elapsed_time_ssl, "s", FALSE, 0, FALSE, 0, FALSE, 0, FALSE, 0);
}

char *perfd_time_headers (double elapsed_time_headers)
{
    return fperfdata ("time_headers", elapsed_time_headers, "s", FALSE, 0, FALSE, 0, FALSE, 0, FALSE, 0);
}

char *perfd_time_firstbyte (double elapsed_time_firstbyte)
{
    return fperfdata ("time_firstbyte", elapsed_time_firstbyte, "s", FALSE, 0, FALSE, 0, FALSE, 0, FALSE, 0);
}

char *perfd_time_transfer (double elapsed_time_transfer)
{
    return fperfdata ("time_transfer", elapsed_time_transfer, "s", FALSE, 0, FALSE, 0, FALSE, 0, FALSE, 0);
}

char *perfd_size (int page_len)
{
    return perfdata ("size", page_len, "B",
                     (min_page_len>0?TRUE:FALSE), min_page_len,
                     (min_page_len>0?TRUE:FALSE), 0,
                     TRUE, 0, FALSE, 0);
}

void
print_help (void)
{
    print_revision (progname, NP_VERSION);

    printf ("Copyright (c) 1999 Ethan Galstad <nagios@nagios.org>\n");
    printf (COPYRIGHT, copyright, email);

    printf ("%s\n", _("This plugin tests the HTTP service on the specified host. It can test"));
    printf ("%s\n", _("normal (http) and secure (https) servers, follow redirects, search for"));
    printf ("%s\n", _("strings and regular expressions, check connection times, and report on"));
    printf ("%s\n", _("certificate expiration times."));

    printf ("\n\n");

    print_usage ();

    printf (_("NOTE: One or both of -H and -I must be specified"));

    printf ("\n");

    printf (UT_HELP_VRSN);
    printf (UT_EXTRA_OPTS);

    printf (" %s\n", "-H, --hostname=ADDRESS");
    printf ("    %s\n", _("Host name argument for servers using host headers (virtual host)"));
    printf ("    %s\n", _("Append a port to include it in the header (eg: example.com:5000)"));
    printf (" %s\n", "-I, --IP-address=ADDRESS");
    printf ("    %s\n", _("IP address or name (use numeric address if possible to bypass DNS lookup)."));
    printf (" %s\n", "-p, --port=INTEGER");
    printf ("    %s", _("Port number (default: "));
    printf ("%d)\n", HTTP_PORT);

    printf (UT_IPv46);

#ifdef HAVE_SSL
    printf (" %s\n", "-S, --ssl=VERSION[+]");
    printf ("    %s\n", _("Connect via SSL. Port defaults to 443. VERSION is optional, and prevents"));
    printf ("    %s\n", _("auto-negotiation (2 = SSLv2, 3 = SSLv3, 1 = TLSv1, 1.1 = TLSv1.1,"));
    printf ("    %s\n", _("1.2 = TLSv1.2). With a '+' suffix, newer versions are also accepted."));
    printf (" %s\n", "--sni");
    printf ("    %s\n", _("Enable SSL/TLS hostname extension support (SNI)"));
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
    printf (" %s\n", "--verify-host");
    printf ("    %s\n", _("Verify SSL certificate is for the -H hostname (with --sni and -S)"));
#endif
    printf (" %s\n", "-C, --certificate=INTEGER[,INTEGER]");
    printf ("    %s\n", _("Minimum number of days a certificate has to be valid. Port defaults to 443"));
    printf ("    %s\n", _("(When this option is used the URL is not checked by default. You can use"));
    printf ("    %s\n", _(" --continue-after-certificate to override this behavior)"));
    printf (" %s\n", "--continue-after-certificate");
    printf ("    %s\n", _("Allows the HTTP check to continue after performing the certificate check."));
    printf ("    %s\n", _("Does nothing unless -C is used."));
    printf (" %s\n", "-J, --client-cert=FILE");
    printf ("   %s\n", _("Name of file that contains the client certificate (PEM format)"));
    printf ("   %s\n", _("to be used in establishing the SSL session"));
    printf (" %s\n", "-K, --private-key=FILE");
    printf ("   %s\n", _("Name of file containing the private key (PEM format)"));
    printf ("   %s\n", _("matching the client certificate"));
#endif

    printf (" %s\n", "-e, --expect=STRING");
    printf ("    %s\n", _("Comma-delimited list of strings, at least one of them is expected in"));
    printf ("    %s", _("the first (status) line of the server response (default: "));
    printf ("%s)\n", HTTP_EXPECT);
    printf ("    %s\n", _("If specified skips all other status line logic (ex: 3xx, 4xx, 5xx processing)"));
    printf (" %s\n", "-d, --header-string=STRING");
    printf ("    %s\n", _("String to expect in the response headers"));
    printf (" %s\n", "-s, --string=STRING");
    printf ("    %s\n", _("String to expect in the content"));
    printf (" %s\n", "-u, --uri=PATH");
    printf ("    %s\n", _("URI to GET or POST (default: /)"));
    printf (" %s\n", "--url=PATH");
    printf ("    %s\n", _("(deprecated) URL to GET or POST (default: /)"));
    printf (" %s\n", "-P, --post=STRING");
    printf ("    %s\n", _("URL encoded http POST data"));
    printf (" %s\n", "-j, --method=STRING  (for example: HEAD, OPTIONS, TRACE, PUT, DELETE, CONNECT)");
    printf ("    %s\n", _("Set HTTP method."));
    printf (" %s\n", "-N, --no-body");
    printf ("    %s\n", _("Don't wait for document body: stop reading after headers."));
    printf ("    %s\n", _("(Note that this still does an HTTP GET or POST, not a HEAD.)"));
    printf (" %s\n", "-M, --max-age=SECONDS");
    printf ("    %s\n", _("Warn if document is more than SECONDS old. the number can also be of"));
    printf ("    %s\n", _("the form \"10m\" for minutes, \"10h\" for hours, or \"10d\" for days."));
    printf (" %s\n", "-T, --content-type=STRING");
    printf ("    %s\n", _("specify Content-Type header media type when POSTing\n"));

    printf (" %s\n", "-l, --linespan");
    printf ("    %s\n", _("Allow regex to span newlines (must precede -r or -R)"));
    printf (" %s\n", "-r, --regex, --ereg=STRING");
    printf ("    %s\n", _("Search page for regex STRING"));
    printf (" %s\n", "-R, --eregi=STRING");
    printf ("    %s\n", _("Search page for case-insensitive regex STRING"));
    printf (" %s\n", "--invert-regex");
    printf ("    %s\n", _("Return CRITICAL if found, OK if not\n"));

    printf (" %s\n", "-a, --authorization=AUTH_PAIR");
    printf ("    %s\n", _("Username:password on sites with basic authentication"));
    printf (" %s\n", "-b, --proxy-authorization=AUTH_PAIR");
    printf ("    %s\n", _("Username:password on proxy-servers with basic authentication"));
    printf (" %s\n", "--proxy");
    printf ("    %s\n", _("Prepend PROXY protocol header"));
    printf (" %s\n", "-A, --useragent=STRING");
    printf ("    %s\n", _("String to be sent in http header as \"User Agent\""));
    printf (" %s\n", "-k, --header=STRING");
    printf ("    %s\n", _("Any other tags to be sent in http header. Use multiple times for additional headers"));
    printf (" %s\n", "-E, --extended-perfdata");
    printf ("    %s\n", _("Print additional performance data"));
    printf (" %s\n", "-o, --output-body-as-perfdata");
    printf ("    %s\n", _("Output response body as performance data on succes"));
    printf (" %s\n", "-U, --show-url");
    printf ("    %s\n", _("Print URL in msg output in plain text"));
    printf (" %s\n", "-L, --link");
    printf ("    %s\n", _("Wrap output in HTML link (obsoleted by urlize)"));
    printf (" %s\n", "-f, --onredirect=<ok|warning|critical|follow|sticky|stickyport>");
    printf ("    %s\n", _("How to handle redirected pages. sticky is like follow but stick to the"));
    printf ("    %s\n", _("specified IP address. stickyport also ensures port stays the same."));
    printf (" %s\n", "-m, --pagesize=INTEGER<:INTEGER>");
    printf ("    %s\n", _("Minimum page size required (bytes) : Maximum page size required (bytes)"));

    printf (UT_WARN_CRIT);

    printf (UT_CONN_TIMEOUT, DEFAULT_SOCKET_TIMEOUT);

    printf (UT_VERBOSE);

    printf ("\n");
    printf ("%s\n", _("Notes:"));
    printf (" %s\n", _("This plugin will attempt to open an HTTP connection with the host."));
    printf (" %s\n", _("Successful connects return STATE_OK, refusals and timeouts return STATE_CRITICAL"));
    printf (" %s\n", _("other errors return STATE_UNKNOWN.  Successful connects, but incorrect response"));
    printf (" %s\n", _("messages from the host result in STATE_WARNING return values.  If you are"));
    printf (" %s\n", _("checking a virtual server that uses 'host headers' you must supply the FQDN"));
    printf (" %s\n", _("(fully qualified domain name) as the [host_name] argument."));
    printf (" %s\n", _("You may also need to give a FQDN or IP address using -I (or --IP-Address)."));

#ifdef HAVE_SSL
    printf ("\n");
    printf (" %s\n", _("This plugin can also check whether an SSL enabled web server is able to"));
    printf (" %s\n", _("serve content (optionally within a specified time) or whether the X509 "));
    printf (" %s\n", _("certificate is still valid for the specified number of days."));
    printf ("\n");
    printf (" %s\n", _("Please note that this plugin does not check if the presented server"));
    printf (" %s\n", _("certificate matches the hostname of the server, or if the certificate"));
    printf (" %s\n", _("has a valid chain of trust to one of the locally installed CAs."));
    printf ("\n");
    printf ("%s\n", _("Examples:"));
    printf (" %s\n\n", "CHECK CONTENT: check_http -w 5 -c 10 --ssl -H www.verisign.com");
    printf (" %s\n", _("When the 'www.verisign.com' server returns its content within 5 seconds,"));
    printf (" %s\n", _("a STATE_OK will be returned. When the server returns its content but exceeds"));
    printf (" %s\n", _("the 5-second threshold, a STATE_WARNING will be returned. When an error occurs,"));
    printf (" %s\n", _("a STATE_CRITICAL will be returned."));
    printf ("\n");
    printf (" %s\n\n", "CHECK CERTIFICATE: check_http -H www.verisign.com -C 14");
    printf (" %s\n", _("When the certificate of 'www.verisign.com' is valid for more than 14 days,"));
    printf (" %s\n", _("a STATE_OK is returned. When the certificate is still valid, but for less than"));
    printf (" %s\n", _("14 days, a STATE_WARNING is returned. A STATE_CRITICAL will be returned when"));
    printf (" %s\n\n", _("the certificate is expired."));
    printf ("\n");
    printf (" %s\n\n", "CHECK CERTIFICATE: check_http -H www.verisign.com -C 30,14");
    printf (" %s\n", _("When the certificate of 'www.verisign.com' is valid for more than 30 days,"));
    printf (" %s\n", _("a STATE_OK is returned. When the certificate is still valid, but for less than"));
    printf (" %s\n", _("30 days, but more than 14 days, a STATE_WARNING is returned."));
    printf (" %s\n", _("A STATE_CRITICAL will be returned when certificate expires in less than 14 days"));

    printf (" %s\n\n", "CHECK SSL WEBSERVER CONTENT VIA PROXY USING HTTP 1.1 CONNECT: ");
    printf (" %s\n", _("check_http -I 192.168.100.35 -p 80 -u https://www.verisign.com/ -S -j CONNECT -H www.verisign.com "));
    printf (" %s\n", _("all these options are needed: -I <proxy> -p <proxy-port> -u <check-url> -S(sl) -j CONNECT -H <webserver>"));
    printf (" %s\n", _("a STATE_OK will be returned. When the server returns its content but exceeds"));
    printf (" %s\n", _("the 5-second threshold, a STATE_WARNING will be returned. When an error occurs,"));
    printf (" %s\n", _("a STATE_CRITICAL will be returned."));

#endif

    printf (UT_SUPPORT);

}



void
print_usage (void)
{
    printf ("%s\n", _("Usage:"));
    printf (" %s -H <vhost> | -I <IP-address> [-u <uri>] [-p <port>]\n",progname);
    printf ("       [-J <client certificate file>] [-K <private key>]\n");
    printf ("       [-w <warn time>] [-c <critical time>] [-t <timeout>] [-L] [-E] [-U] [-a auth]\n");
    printf ("       [-b proxy_auth] [--proxy] [-f <ok|warning|critical|follow|sticky|stickyport>]\n");
    printf ("       [-e <expect>] [-d string] [-s string] [-l] [-r <regex> | -R <case-insensitive regex>]\n");
    printf ("       [-P string] [-m <min_pg_size>:<max_pg_size>] [-4|-6] [-N] [-M <age>]\n");

#if OPENSSL_VERSION_NUMBER >= 0x10002000L
    printf ("       [-A string] [-k string] [-S <version>] [--sni] [--verify-host]\n");
    printf ("       [-C <warn_age>[,<crit_age>]] [-T <content-type>] [-j method]\n");
#else
    printf ("       [-A string] [-k string] [-S <version>] [--sni] [-C <warn_age>[,<crit_age>]]\n");
    printf ("       [-T <content-type>] [-j method]\n");
#endif
}
