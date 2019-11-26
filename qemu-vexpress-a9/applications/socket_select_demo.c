#include "sys/socket.h"
#include "lwip/sys.h"
#include "rtthread.h"
#include <string.h>


#define MAX_SERV                    5               /* Maximum number of chargen services. Don't need too many */
#define CHARGEN_THREAD_NAME         "chargen"
#define CHARGEN_THREAD_STACKSIZE    4096
#define SEND_SIZE                   TCP_SNDLOWAT    /* If we only send this much, then when select says we can send, we know we won't block */

struct charcb 
{
    struct charcb *next;
    int socket;
    struct sockaddr_in cliaddr;
    socklen_t clilen;
    char nextchar;
};

static struct charcb *charcb_list = 0;
static int do_read(struct charcb *p_charcb);
static void close_chargen(struct charcb *p_charcb);

/*
 * chargen task. This server will wait for connections on well
 * known TCP port number: 19. For every connection, the server will
 * write as much data as possible to the tcp port.
 */
static void chargen_thread(void *arg)
{
    int listenfd;
    struct sockaddr_in chargen_saddr;
    fd_set readset;
    fd_set writeset;
    int i, maxfdp1;
    struct charcb *p_charcb;

    /* First acquire our socket for listening for connections */
    listenfd = socket(AF_INET, SOCK_STREAM, 0);

    LWIP_ASSERT("chargen_thread(): Socket create failed.", listenfd >= 0);
    rt_memset(&chargen_saddr, 0, sizeof(chargen_saddr));
    chargen_saddr.sin_family = AF_INET;
    chargen_saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    chargen_saddr.sin_port = htons(19);     /* Chargen server port */

    if (bind(listenfd, (struct sockaddr *) &chargen_saddr, sizeof(chargen_saddr)) == -1)
        LWIP_ASSERT("chargen_thread(): Socket bind failed.", 0);

    /* Put socket into listening mode */
    if (listen(listenfd, MAX_SERV) == -1)
        LWIP_ASSERT("chargen_thread(): Listen failed.", 0);

    /* Wait forever for network input: This could be connections or data */
    for (;;)
    {
        maxfdp1 = listenfd+1;

        /* Determine what sockets need to be in readset */
        FD_ZERO(&readset);
        FD_ZERO(&writeset);
        FD_SET(listenfd, &readset);
        for (p_charcb = charcb_list; p_charcb; p_charcb = p_charcb->next)
        {
                if (maxfdp1 < p_charcb->socket + 1)
                    maxfdp1 = p_charcb->socket + 1;
                FD_SET(p_charcb->socket, &readset);
                FD_SET(p_charcb->socket, &writeset);
        }
        
        /* Wait for data or a new connection */
        i = select(maxfdp1, &readset, &writeset, 0, 0);
        
        if (i == 0)
            continue;
        /* At least one descriptor is ready */
        if (FD_ISSET(listenfd, &readset))
        {
            /* We have a new connection request!!! */
            /* Lets create a new control block */
            p_charcb = (struct charcb *)rt_malloc(sizeof(struct charcb));
            if (p_charcb)
            {
                p_charcb->socket = accept(listenfd,
                                        (struct sockaddr *) &p_charcb->cliaddr,
                                        &p_charcb->clilen);
                if (p_charcb->socket < 0)
                    rt_free(p_charcb);
                else
                {
                    /* Keep this tecb in our list */
                    p_charcb->next = charcb_list;
                    charcb_list = p_charcb;
                    p_charcb->nextchar = 0x41;
                }
            } else {
                /* No memory to accept connection. Just accept and then close */
                int sock;
                struct sockaddr cliaddr;
                socklen_t clilen;

                sock = accept(listenfd, &cliaddr, &clilen);
                if (sock >= 0)
                    closesocket(sock);
            }
        }
        /* Go through list of connected clients and process data */
        for (p_charcb = charcb_list; p_charcb; p_charcb = p_charcb->next)
        {
            if (FD_ISSET(p_charcb->socket, &readset))
            {
                /* This socket is ready for reading. This could be because someone typed
                 * some characters or it could be because the socket is now closed. Try reading
                 * some data to see. */
                if (do_read(p_charcb) < 0)
                    break;
            }
            if (FD_ISSET(p_charcb->socket, &writeset))
            {
                char line[80];
                char setchar = p_charcb->nextchar;

                for( i = 0; i < 59; i++)
                {
                    line[i] = setchar;
                    if (++setchar == 0x7f)
                        setchar = 0x41;
                }
                line[i] = 0;
                strcat(line, "\n\r");
                if (write(p_charcb->socket, line, strlen(line)) < 0)
                {
                    close_chargen(p_charcb);
                    break;
                }
                if (++p_charcb->nextchar == 0x7f)
                    p_charcb->nextchar = 0x41;
            }     
        }
    }  
}

/*
 * Close the socket and remove this charcb from the list.
*/
static void close_chargen(struct charcb *p_charcb)
{
    struct charcb *p_search_charcb;

        /* Either an error or tcp connection closed on other
         * end. Close here */
        closesocket(p_charcb->socket);
        /* Free charcb */
        if (charcb_list == p_charcb)
            charcb_list = p_charcb->next;
        else
            for (p_search_charcb = charcb_list; p_search_charcb; p_search_charcb = p_search_charcb->next)
            {
                if (p_search_charcb->next == p_charcb)
                {
                    p_search_charcb->next = p_charcb->next;
                    break;
                }
            }
        rt_free(p_charcb);
}

/*
 * Socket definitely is ready for reading. Read a buffer from the socket and
 * discard the data.  If no data is read, then the socket is closed and the
 * charcb is removed from the list and freed.
*/
static int do_read(struct charcb *p_charcb)
{
    char buffer[80];
    int readcount;

    /* Read some data */
    readcount = read(p_charcb->socket, &buffer, 80);
    if (readcount <= 0)
    {
        close_chargen(p_charcb);
        return -1;
    }
	rt_kprintf("recv data len = %d\n", readcount);
    return 0;
}

/*
 * This function initializes the chargen service. This function
 * may only be called either before or after tasking has started.
 */
void chargen_init(void)
{
    sys_thread_new(CHARGEN_THREAD_NAME, chargen_thread, NULL, CHARGEN_THREAD_STACKSIZE, TCPIP_THREAD_PRIO+1);
    rt_kprintf("Startup a tcp concurrent server.\n");
}
MSH_CMD_EXPORT_ALIAS(chargen_init, select_demo, Start a char generator using select);