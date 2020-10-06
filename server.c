/*
 * server.c
 */

#include <stdio.h>      /* for printf() and fprintf() */
#include <sys/socket.h> /* for socket(), bind(), and connect() */
#include <arpa/inet.h>  /* for sockaddr_in and inet_ntoa() */
#include <stdlib.h>     /* for atoi() and exit() */
#include <string.h>     /* for memset() */
#include <unistd.h>     /* for close() */
#include <time.h>       /* for time() */
#include <netdb.h>      /* for gethostbyname() */
#include <signal.h>     /* for signal() */
#include <sys/stat.h>   /* for stat() */
#include <sys/wait.h>  /* for waitpid() */
#include <sys/shm.h>    /* for shmget()*/
#include <sys/types.h>  /* for key_t */
#include <semaphore.h>  /* for sem_t */
#include <pthread.h>   /* for pthread */
#include <errno.h>      /* for errno */

#define MAXPENDING 5    /* Maximum outstanding connection requests */

#define DISK_IO_BUF_SIZE 4096

#define READ_WRITE_MODE 0600

#define N_CHILD 16

struct stats {

    int total;
    int twoHundreds;
    int threeHundreds;
    int fourHundreds; 
    int fiveHundreds;
    /* Semaphore should be put in the struct because when the process forks the
     * child will get a copy of the semaphore. Therefore it won't function as
     * intended. By putting it in the struct, it is located on the in the shared
     * mem
     */
    sem_t sem;
};

struct stats *statsPtr;

void updateStats(int, struct stats *);
void sendStatsPage(int clntSock, struct stats *statsPtr);

static void sendConnection(int clntSock, int sock);
static int recvConnection(int sock);

static void die(const char *message)
{
    perror(message);
    exit(1); 
}

/*
 * Create a listening socket bound to the given port.
 */
static int createServerSocket(unsigned short port)
{
    int servSock;
    struct sockaddr_in servAddr;

    /* Create socket for incoming connections */
    if ((servSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
        die("socket() failed");
      
    /* Construct local address structure */
    memset(&servAddr, 0, sizeof(servAddr));       /* Zero out structure */
    servAddr.sin_family = AF_INET;                /* Internet address family */
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY); /* Any incoming interface */
    servAddr.sin_port = htons(port);              /* Local port */

    /* Bind to the local address */
    if (bind(servSock, (struct sockaddr *)&servAddr, sizeof(servAddr)) < 0)
        die("bind() failed");

    /* Mark the socket so it will listen for incoming connections */
    if (listen(servSock, MAXPENDING) < 0)
        die("listen() failed");

    return servSock;
}

/*
 * A wrapper around send() that does error checking and logging.
 * Returns -1 on failure.
 * 
 * This function assumes that buf is a null-terminated string, so
 * don't use this function to send binary data.
 */
ssize_t Send(int sock, const char *buf)
{
    size_t len = strlen(buf);
    ssize_t res = send(sock, buf, len, 0);
    if (res != len) {
        perror("send() failed");
        return -1;
    }
    else 
        return res;
}

/*
 * HTTP/1.0 status codes and the corresponding reason phrases.
 */

static struct {
    int status;
    char *reason;
} HTTP_StatusCodes[] = {
    { 200, "OK" },
    { 201, "Created" },
    { 202, "Accepted" },
    { 204, "No Content" },
    { 301, "Moved Permanently" },
    { 302, "Moved Temporarily" },
    { 304, "Not Modified" },
    { 400, "Bad Request" },
    { 401, "Unauthorized" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 500, "Internal Server Error" },
    { 501, "Not Implemented" },
    { 502, "Bad Gateway" },
    { 503, "Service Unavailable" },
    { 0, NULL } // marks the end of the list
};

static inline const char *getReasonPhrase(int statusCode)
{
    int i = 0;
    while (HTTP_StatusCodes[i].status > 0) {
        if (HTTP_StatusCodes[i].status == statusCode)
            return HTTP_StatusCodes[i].reason;
        i++;
    }
    return "Unknown Status Code";
}


/*
 * Send HTTP status line followed by a blank line.
 */
static void sendStatusLine(int clntSock, int statusCode)
{
    char buf[1000];
    const char *reasonPhrase = getReasonPhrase(statusCode);

    // print the status line into the buffer
    sprintf(buf, "HTTP/1.0 %d ", statusCode);
    strcat(buf, reasonPhrase);
    strcat(buf, "\r\n");

    // We don't send any HTTP header in this simple server.
    // We need to send a blank line to signal the end of headers.
    strcat(buf, "\r\n");

    // For non-200 status, format the status line as an HTML content
    // so that browers can display it.
    if (statusCode != 200) {
        char body[1000];
        sprintf(body, 
                "<html><body>\n"
                "<h1>%d %s</h1>\n"
                "</body></html>\n",
                statusCode, reasonPhrase);
        strcat(buf, body);
    }

    // send the buffer to the browser
    Send(clntSock, buf);
}


static void sendDirectoryListing(int clntSock, const char *path)
{
    int fd[2];
    pid_t pid;
    char line[4096];
    int n;

    if (pipe(fd) < 0)
        die("pipe failed");

    pid = fork();
    if (pid < 0) {
        die("fork failed");
    } else if (pid == 0) {
        close(fd[0]);
        dup2(fd[1], STDOUT_FILENO);
        dup2(fd[1], STDERR_FILENO); // redirects stderr
        execl("/bin/ls", "ls", "-al", path, NULL);
        die("execl failed");
    } else {
        close(fd[1]);
        while ((n = read(fd[0], line, 4095)) > 0) {
            //fprintf(stderr, "%lu %d\n", strlen(line), n);
            line[n] = '\0';
            Send(clntSock, line);
        }
        //write(STDOUT_FILENO, line, n);
    }
}


static void *sendStatsSignal(void *arg)
{
    sem_wait(&statsPtr->sem);

    char buf[1000];
    int n = sprintf(buf, 
            "/statistics\nRequests:\t%d\n\n2xx:\t%d\n3xx:\t%d\n4xx:\t%d\n5xx:\t%d\n",
            statsPtr->total, 
            statsPtr->twoHundreds, 
            statsPtr->threeHundreds, 
            statsPtr->fourHundreds, 
            statsPtr->fiveHundreds);

    sem_post(&statsPtr->sem);

    write(STDERR_FILENO, buf, n);

    return NULL;
}


static int handleFileRequest(
        const char *webRoot, const char *requestURI, int clntSock)
{
    int statusCode;
    FILE *fp = NULL;

    printf("web:%s\nrequest:%s\n", webRoot, requestURI);
    // Compose the file path from webRoot and requestURI.
    // If requestURI ends with '/', append "index.html".

    char *file = (char *)malloc(strlen(webRoot) + strlen(requestURI) + 100);
    if (file == NULL)
        die("malloc failed");
    strcpy(file, webRoot);
    strcat(file, requestURI);
    if (file[strlen(file)-1] == '/') {
        strcat(file, "index.html");
    }

    // See if the requested file is a directory.
    // Our server does not support directory listing.

    struct stat st;
    if (stat(file, &st) == 0 && S_ISDIR(st.st_mode)) {
        //statusCode = 403; // "Forbidden"
        //sendStatusLine(clntSock, statusCode);
        statusCode = 200; // "OK"
        sendStatusLine(clntSock, statusCode);
        sendDirectoryListing(clntSock, requestURI);
        goto func_end;
    }

    // If unable to open the file, send "404 Not Found".

    fp = fopen(file, "rb");
    if (fp == NULL) {
        statusCode = 404; // "Not Found"
        sendStatusLine(clntSock, statusCode);
        goto func_end;
    }

    // Otherwise, send "200 OK" followed by the file content.

    statusCode = 200; // "OK"
    sendStatusLine(clntSock, statusCode);

    // send the file 
    size_t n;
    char buf[DISK_IO_BUF_SIZE];
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (send(clntSock, buf, n, 0) != n) {
            // send() failed.
            // We log the failure, break out of the loop,
            // and let the server continue on with the next request.
            perror("\nsend() failed");
            break;
        }
    }
    // fread() returns 0 both on EOF and on error.
    // Let's check if there was an error.
    if (ferror(fp))
        perror("fread failed");

func_end:

    // clean up
    free(file);
    if (fp)
        fclose(fp);

    return statusCode;
}


/*
 * Each child process acts as its own web server and waits for accept in an infinite
 * loop.
 */
void process_request(int sock, const char* webRoot) {

    struct sockaddr_in clntAddr;
    char line[1000];
    char requestLine[1000];
    int statusCode;

    for (;;) {

        /*
         * wait for a client to connect
         */

        // initialize the in-out parameter
        
        int clntSock = recvConnection(sock);
        if (clntSock < 0)
            die("recvConnection() failed");

        FILE *clntFp = fdopen(clntSock, "r");
        if (clntFp == NULL)
            die("fdopen failed");

        //FILE *clntFp = recvConnection(sock);

        /*
         * Let's parse the request line.
         */

        char *method      = "";
        char *requestURI  = "";
        char *httpVersion = "";

        if (fgets(requestLine, sizeof(requestLine), clntFp) == NULL) {
            // socket closed - there isn't much we can do
            statusCode = 400; // "Bad Request"
            goto loop_end;
        }

        char *token_separators = "\t \r\n"; // tab, space, new line
        method = strtok(requestLine, token_separators);
        requestURI = strtok(NULL, token_separators);
        httpVersion = strtok(NULL, token_separators);
        char *extraThingsOnRequestLine = strtok(NULL, token_separators);

        // check if we have 3 (and only 3) things in the request line
        if (!method || !requestURI || !httpVersion || 
                extraThingsOnRequestLine) {
            statusCode = 501; // "Not Implemented"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }

        // we only support GET method 
        if (strcmp(method, "GET") != 0) {
            statusCode = 501; // "Not Implemented"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }

        // we only support HTTP/1.0 and HTTP/1.1
        if (strcmp(httpVersion, "HTTP/1.0") != 0 && 
            strcmp(httpVersion, "HTTP/1.1") != 0) {
            statusCode = 501; // "Not Implemented"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }
        
        // requestURI must begin with "/"
        if (!requestURI || *requestURI != '/') {
            statusCode = 400; // "Bad Request"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }

        // make sure that the requestURI does not contain "/../" and 
        // does not end with "/..", which would be a big security hole!
        int len = strlen(requestURI);
        if (len >= 3) {
            char *tail = requestURI + (len - 3);
            if (strcmp(tail, "/..") == 0 || 
                    strstr(requestURI, "/../") != NULL)
            {
                statusCode = 400; // "Bad Request"
                sendStatusLine(clntSock, statusCode);
                goto loop_end;
            }
        }

        //Check to see if that stats page is requested 
            if (strcmp(requestURI, "/statistics") == 0)
            {
                sendStatsPage(clntSock, statsPtr);
                goto loop_end;
            }

        /*
         * Now let's skip all headers.
         */

        while (1) {
            if (fgets(line, sizeof(line), clntFp) == NULL) {
                // socket closed prematurely - there isn't much we can do
                statusCode = 400; // "Bad Request"
                goto loop_end;
            }
            if (strcmp("\r\n", line) == 0 || strcmp("\n", line) == 0) {
                // This marks the end of headers.  
                // Break out of the while loop.
                break;
            }
        }

        /*
         * At this point, we have a well-formed HTTP GET request.
         * Let's handle it.
         */

        statusCode = handleFileRequest(webRoot, requestURI, clntSock);

        loop_end:

        //Access shared memory and update the error codes and the total

        updateStats(statusCode, statsPtr);

        /*
         * Done with client request.
         * Log it, close the client socket, and go back to accepting
         * connection.
         */
        
        fprintf(stderr, "[PID=%d] %s \"%s %s %s\" %d %s\n",
                getpid(),
                inet_ntoa(clntAddr.sin_addr),
                method,
                requestURI,
                httpVersion,
                statusCode,
                getReasonPhrase(statusCode));

        // close the client socket 
        fclose(clntFp);

        } 

}


void handle_sigusr1(int signal) {
    
    pthread_t t1;
    pthread_create(&t1, NULL, sendStatsSignal, (void*)1); 
    pthread_join(t1, NULL);
}


// Send clntSock through sock.
// sock is a UNIX domain socket.
static void sendConnection(int clntSock, int sock)
{
    struct msghdr msg;
    struct iovec iov[1];

    union {
      struct cmsghdr cm;
      char control[CMSG_SPACE(sizeof(int))];
    } ctrl_un;
    struct cmsghdr *cmptr;

    msg.msg_control = ctrl_un.control;
    msg.msg_controllen = sizeof(ctrl_un.control);

    cmptr = CMSG_FIRSTHDR(&msg);
    cmptr->cmsg_len = CMSG_LEN(sizeof(int));
    cmptr->cmsg_level = SOL_SOCKET;
    cmptr->cmsg_type = SCM_RIGHTS;
    *((int *) CMSG_DATA(cmptr)) = clntSock;

    msg.msg_name = NULL;
    msg.msg_namelen = 0;

    iov[0].iov_base = "FD";
    iov[0].iov_len = 2;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    if (sendmsg(sock, &msg, 0) != 2)
        die("Failed to send connection to child");
}

// Returns an open file descriptor received through sock.
// sock is a UNIX domain socket.
static int recvConnection(int sock)
{
    struct msghdr msg;
    struct iovec iov[1];
    ssize_t n;
    char buf[64];

    union {
      struct cmsghdr cm;
      char control[CMSG_SPACE(sizeof(int))];
    } ctrl_un;
    struct cmsghdr *cmptr;

    msg.msg_control = ctrl_un.control;
    msg.msg_controllen = sizeof(ctrl_un.control);

    msg.msg_name = NULL;
    msg.msg_namelen = 0;

    iov[0].iov_base = buf;
    iov[0].iov_len = sizeof(buf);
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    for (;;) {
        n = recvmsg(sock, &msg, 0);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            die("Error in recvmsg");
        }
        // Messages with client connections are always sent with 
        // "FD" as the message. Silently skip unsupported messages.
        if (n != 2 || buf[0] != 'F' || buf[1] != 'D')
            continue;

        if ((cmptr = CMSG_FIRSTHDR(&msg)) != NULL
            && cmptr->cmsg_len == CMSG_LEN(sizeof(int))
            && cmptr->cmsg_level == SOL_SOCKET
            && cmptr->cmsg_type == SCM_RIGHTS)
            return *((int *) CMSG_DATA(cmptr));
    }
}


int main(int argc, char *argv[])
{

    if (signal(SIGUSR1, SIG_IGN) == SIG_ERR)
        die("signal() failed");

    // Ignore SIGPIPE so that we don't terminate when we call
    // send() on a disconnected socket.
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        die("signal() failed");

    if (argc != 3) {
        fprintf(stderr, "usage: %s <server_port> <web_root>\n", argv[0]);
        exit(1);
    }


    int shmid;
    //struct stats *statsPtr;

    //Create or get the shared mem
    if ((shmid = shmget(IPC_PRIVATE, sizeof(struct stats), READ_WRITE_MODE)) < 0) {
        perror("shmget");
        exit(1);
    }

    //Attach the shared mem to process
    if((statsPtr = shmat(shmid, 0, 0)) == (void *) -1){
        perror("shmat");
        exit(1);
    }
    //Init the semaphore
    if((sem_init(&(statsPtr->sem), 0, 1))  < 0) {
        perror("semaphore init problem");
        exit(1);
    }


    unsigned short servPort = atoi(argv[1]);
    const char *webRoot = argv[2];

    // created server socket before creating child processes
    // because if bind() fails, all of the children become zombies
    int servSock = createServerSocket(servPort);

    pid_t pid;
    int i = 0;

    int parentSockets[N_CHILD];

    // Loop N_CHILD times to create that many children.
    while(i < N_CHILD) {

        int fd[2];
        socketpair(PF_LOCAL, SOCK_STREAM, 0, fd);

        pid = fork();

        if(pid < 0) {
            die("fork failed");
        }

        // Launch the child web server.
        // Exit so that we do not fork on the child process.
        else if(pid == 0) {

            close(fd[0]);

            process_request(fd[1], webRoot);
            exit(0);
        }

        // Add child to array and fork again. 
        else {

            close(fd[1]);

            parentSockets[i++] = fd[0];
            //fprintf(stderr, "ADDED FILE DESCRIPTOR: %d\n", fd[0]);
            //fprintf(stderr, "CONFIRMED FILE DESCRIPTOR: %d\n", parentSockets[i]);
        }
    }


    struct sigaction stats_action;
    stats_action.sa_handler = handle_sigusr1;
    sigemptyset(&stats_action.sa_mask);
    stats_action.sa_flags = SA_RESTART;

    if (sigaction(SIGUSR1, &stats_action, NULL) == -1)
        die("signal() failed");


    struct sockaddr_in clntAddr;
    unsigned int clntLen = sizeof(clntAddr); 

    while (1) {

        for (i = 0; i < N_CHILD; i++) {

            //fprintf(stderr, "ABOUT TO SEND THROUGH PARENT FD: %d\n", parentSockets[i]);

            int clntSock = accept(servSock, (struct sockaddr *)&clntAddr, &clntLen);
            if (clntSock < 0)
                die("accept() failed");

            sendConnection(clntSock, parentSockets[i]);

            close(clntSock);

            //fprintf(stderr,"Sent through parent socket number %d!\n", i);

        }
    }


    /*while((pid = waitpid( (pid_t) -1, NULL, 0)) > 0) {
        // waits for any child process 
    }*/

    
    return 0;
}


//TODO: Capture the termination signal and free shared mem address when caught

void updateStats(int code, struct stats *statsPtr){
    //Lock the semaphore
    sem_wait(&statsPtr->sem);
    //Update the states
    if((code >= 200) && (code <= 299)){
        statsPtr->twoHundreds++;
        statsPtr->total++;
    }
    if((code >= 300) && (code <= 399)){
        statsPtr->threeHundreds++;
        statsPtr->total++;
    }
    if((code >= 400) && (code <= 499)){
        statsPtr->fourHundreds++;
        statsPtr->total++;
    }
    if((code >= 500) && (code <= 599)){
        statsPtr->fiveHundreds++;
        statsPtr->total++;
    }
    //Unlock the semaphore  
    sem_post(&statsPtr->sem);
    //printf("/statistics\nRequests:\t%d\n\n2xx:\t%d\n3xx:\t%d\n4xx:\t%d\n5xx:\t%d\n", statsPtr->total, statsPtr->twoHundreds, statsPtr->threeHundreds, statsPtr->fourHundreds, statsPtr->fiveHundreds);
}

void sendStatsPage(int clntSock, struct stats *statsPtr){
    //Send the header 
    //sendStatusLine(clntSock, 200);
    //Send the webpage 
    char body[1000];
    sprintf(body, 
            "/statistics\nRequests:\t%d\n\n2xx:\t%d\n3xx:\t%d\n4xx:\t%d\n5xx:\t%d\n",
            statsPtr->total, statsPtr->twoHundreds, statsPtr->threeHundreds, statsPtr->fourHundreds, statsPtr->fiveHundreds);
    Send(clntSock, body);
}

