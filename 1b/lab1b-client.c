#include <string.h>
#include <termios.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <mcrypt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>

//
#define BUFFERSIZE 256

// Pipe structure
#define WRITE_END 1
#define READ_END 0

// Poll fd array
#define KEYBOARD 0
#define SHELL 1

#define KEY_LENGTH 16
/* --- System calls with error handling --- */
int safeRead(int fd, char *buffer, ssize_t size)
{
    int status = read(fd, buffer, size);
    if (status < 0)
    {
        fprintf(stderr, "Unable to read from input %s\n", strerror(errno));
        exit(1);
    }

    return status;
}

int safeWrite(int fd, char *buffer, ssize_t size)
{
    int status = write(fd, buffer, size);
    if (status != size)
    {
        fprintf(stderr, "Unable to write to STDOUT %s\n", strerror(errno));
        exit(1);
    }

    return status;
}

void safeDup2(int original_fd, int new_fd)
{
    if (dup2(original_fd, new_fd) < 0)
    {
        fprintf(stderr, "Dup2 failed: %s\n", strerror(errno));
        exit(1);
    }
}

void safeClose(int fd)
{
    if (close(fd) < 0)
    {
        fprintf(stderr, "Error closing file: %s\n", strerror(errno));
        exit(1);
    }
}

void safeKill(int processID, int SIGNUM)
{
    if (kill(processID, SIGNUM) < 0)
    {
        fprintf(stderr, "Kill failed: %s\n", strerror(errno));
        exit(1);
    }
}


// Global Variables
struct termios defaultMode;
char readBuffer[BUFFERSIZE];
int portNum;
int encryptFlag, portFlag, logFlag;
struct pollfd clientPollArray[2];
int processID;
struct sockaddr_in serverAddress;
struct hostent *server;
int socketfd;
MCRYPT cipher, decipher;
char *encryptionKey;
int keyLength;
struct stat keyStat;



/* --- Encyrption functions --- */
void encrypt(char *buffer, int cryptLength)
{
    if (mcrypt_generic(cipher, buffer, cryptLength) != 0)
    {
        fprintf(stderr, "Encryption error: %s", strerror(errno));
        exit(1);
    }
}

void decrypt(char *buffer, int decryptLength)
{

    if (mdecrypt_generic(cipher, buffer, decryptLength) != 0)
    {
        fprintf(stderr, "Decryption error: %s", strerror(errno));
        exit(1);
    }
}

void setupEncryption(char *key, int keyLength)
{
    cipher = mcrypt_module_open("blowfish", NULL, "ofb", NULL);
    if (cipher == MCRYPT_FAILED)
    {
        fprintf(stderr, "Module opening error: %s", strerror(errno));
        exit(1);
    }
    if (mcrypt_generic_init(cipher, key, keyLength, NULL) < 0)
    {
        fprintf(stderr, "MCRYPT initialization error: %s", strerror(errno));
        exit(1);
    }

    decipher = mcrypt_module_open("blowfish", NULL, "ofb", NULL);
    if (decipher == MCRYPT_FAILED)
    {
        fprintf(stderr, "Module opening error: %s", strerror(errno));
        exit(1);
    }
    if (mcrypt_generic_init(decipher, key, keyLength, NULL) < 0)
    {
        fprintf(stderr, "Decryption error: %s", strerror(errno));
        exit(1);
    }
}

void encryptionShutDown()
{
	mcrypt_generic_deinit(cipher);
	mcrypt_module_close(cipher);

	mcrypt_generic_deinit(decipher);
	mcrypt_module_close(decipher);
}

/* --- Utility Functions --- */
// Restores to pre-execution environment: frees memory, closes pipes etc.
void restore()
{
    //Free Memory
    free(encryptionKey);

    //Reset terminal mode
    tcsetattr(STDIN_FILENO, TCSANOW, &defaultMode);

    //De-initialize encryption tools
    if (encryptFlag > 0)
        encryptionShutDown();

}

/* --- Primary Functions --- */
// Writes numBytes worth of data from a given buffer
// Deals with special character according to format - specified in meta
void writeBytes(int numBytes, int writeFD, char *buffer)
{
    int displacement;
    for (displacement = 0; displacement < numBytes; displacement++)
    {
        char *charPosition = buffer + displacement;
        switch (*(charPosition))
        {
        case '\r':
        case '\n':
            if (writeFD == STDOUT_FILENO)
                safeWrite(writeFD, "\r\n", 2);
            else
                safeWrite(writeFD, charPosition, 1);
            break;
        //C-d
        case 4:
            safeWrite(writeFD, charPosition, 1);
            break;
        //C-c
        case 3:
            safeWrite(writeFD, charPosition, 1);
            break;
        default:
            safeWrite(writeFD, charPosition, 1);
            break;
        }
    }
}

// Polls pipeFromShell and pipeToShell 's read ends for input and interrupts
void readOrPoll(struct pollfd *pollArray, char *readBuffer)
{
    //Infinite loop to keep reading and/or polling
    while (1)
    {
        int pendingReads = poll(pollArray, 2, 0);
        int numBytes = 0;

        //Polling sys call failure
        if (pendingReads < 0)
        {
            fprintf(stderr, "Polling error: %s\n", strerror(errno));
            exit(1);
        }

        //Nothing to poll - no reads needed go ahead, to next iteration
        if (pendingReads == 0)
            continue;

        //.revents --> returns bits of events that occured, hence using bit manipulation...
        //Keyboard has data to be read
        if (pollArray[KEYBOARD].revents & POLLIN)
        {

            //Data has been sent in from keyboard, need to read it
            numBytes = safeRead(STDIN_FILENO, readBuffer, BUFFERSIZE);

            //Data in from actual keyboard, must be echoed to screen
            writeBytes(numBytes, STDOUT_FILENO, readBuffer);

            //Encrypt Buffer
            if (encryptFlag > 0)
            {
                int eCount;
                for (eCount = 0; eCount < numBytes; eCount++)
                    encrypt(readBuffer + eCount, 1);
            }
                

            if (logFlag > 0)
            {
                dprintf(logFlag, "SENT %d bytes: ", numBytes);
                writeBytes(numBytes, logFlag, readBuffer);
                dprintf(logFlag, "\n");
            }

            // Reset format for writing to socket
            //Data must be written to socket to communicate
            writeBytes(numBytes, socketfd, readBuffer);
        }

        //Shell has output to be reads
        if (pollArray[SHELL].revents & POLLIN)
        {

            // Server has output to be read
            numBytes = safeRead(socketfd, readBuffer, BUFFERSIZE);
            
            //Skip ahead if 0 bytes
            if (numBytes == 0)
            {
                safeClose(socketfd);
                exit(0);
            }

            if (logFlag > 0)
            {
                dprintf(logFlag, "RECEIVED %d bytes: ", numBytes);
                writeBytes(numBytes, logFlag, readBuffer);
                dprintf(logFlag, "\n");
            }

            //Decrypt Buffer
            if (encryptFlag > 0)
            {
                int dCount;
                for (dCount = 0; dCount < numBytes; dCount++)
                    decrypt(readBuffer + dCount, 1);
            }

            //Data has been sent over from server, need to display to screen
            writeBytes(numBytes, STDOUT_FILENO, readBuffer);
        }

        if (pollArray[SHELL].revents & (POLLHUP | POLLERR))
            exit(1);
    }
    return;
}

int main(int argc, char *argv[])
{
    //Options
    int option = 1;
    encryptFlag = 0;
    logFlag = 0;
    portFlag = 0;

    static struct option shell_options[] = {
        {"port", required_argument, 0, 'p'},
        {"encrypt", required_argument, 0, 'e'},
        {"log", required_argument, 0, 'l'}};

    while ((option = getopt_long(argc, argv, "p:el", shell_options, NULL)) > -1)
    {
        switch (option)
        {
        case 'p':
            portFlag = 1;
            portNum = atoi(optarg);
            break;
        case 'e':
            encryptFlag = open(optarg, O_RDONLY);
            if (encryptFlag < 0)
            {
                fprintf(stderr, "Opening error: %s\n", strerror(errno));
                exit(1);
            }
            else
            {
                if (fstat(encryptFlag, &keyStat) < 0)
                {
                    fprintf(stderr, "Fstat error: %s", strerror(errno));
                    exit(1);
                }
                if ((encryptionKey = malloc(keyStat.st_size * sizeof(char))) == NULL)
                {
                    fprintf(stderr, "Memory allocation error: %s", strerror(errno));
                    exit(1);
                }
                keyLength = safeRead(encryptFlag, encryptionKey, keyStat.st_size * sizeof(char));
                safeClose(encryptFlag);
            }
            break;
        case 'l':
            logFlag = creat(optarg, S_IRWXU);
            if (logFlag < 0)
            {
                fprintf(stderr, "Opening error: %s\n", strerror(errno));
                exit(1);
            }
            break;

        default:
            fprintf(stderr, "Usage: %s --port=portNum [--encrypt=file.key] [--log]\n", argv[0]);
            exit(1);
        }
    }

    //Check for port flag, else abort with usage message
    if (portFlag < 0)
    {
        fprintf(stderr, "Port option is mandatory. Usage: %s --port=portNum [--encrypt=file.key] [--log]\n", argv[0]);
        exit(1);
    }

    //Get terminal paramters
    tcgetattr(STDIN_FILENO, &defaultMode);

    //Make copy of current terminal mode
    struct termios projectMode = defaultMode;

    //Replace terminal parameters
    projectMode.c_iflag = ISTRIP;
    projectMode.c_oflag = 0;
    projectMode.c_lflag = 0;

    //Set up new non-canonical input mode with no echo
    //TCSANOW opt does the action immediately
    if (tcsetattr(0, TCSANOW, &projectMode) < 0)
    {
        fprintf(stderr, "Unable to set non-canonical input mode with no echo: %s", strerror(errno));
        exit(1);
    }

    atexit(restore);

    //Set up connection to socket to communicate with server
    socketfd = socket(AF_INET, SOCK_STREAM, 0); // Create socket
    if (socketfd < 0)
    {
        fprintf(stderr, "Socket creation error: %s\n", strerror(errno));
        exit(1);
    }

    server = gethostbyname("localhost"); // Get host IP address etc.
    if (server == NULL)
    {
        fprintf(stderr, "Error no such host: %s\n", strerror(errno));
        exit(1);
    }

    bzero((char *)&serverAddress, sizeof(serverAddress));                                    //zero out server address
    serverAddress.sin_family = AF_INET;                                                      //
    bcopy((char *)server->h_addr, (char *)&serverAddress.sin_addr.s_addr, server->h_length); //Copies contents of server->h_addr into serverAddress field
    serverAddress.sin_port = htons(portNum);

    if (connect(socketfd, (struct sockaddr *)&serverAddress, sizeof(serverAddress)) < 0) //connecting to server
    {
        fprintf(stderr, "Socket connection error: %s\n", strerror(errno));
        exit(1);
    }

    bzero(readBuffer, BUFFERSIZE);

    //Make non-blocking socket for I/O
    int flags = fcntl(0, F_GETFL);
    fcntl(socketfd, F_SETFL, flags | O_NONBLOCK);

    //Set up polling
    //For input from keyboard
    clientPollArray[KEYBOARD].fd = STDIN_FILENO;
    clientPollArray[KEYBOARD].events = POLLIN | POLLHUP | POLLERR;

    //For output from shell (coming through server)
    clientPollArray[SHELL].fd = socketfd;
    clientPollArray[SHELL].events = POLL_IN | POLL_HUP | POLL_ERR;

    //setup encryption if necessary
    if (encryptFlag > 0)
        setupEncryption(encryptionKey, keyLength);

    //Give over handling to Utility Function readOrPoll
    readOrPoll(clientPollArray, readBuffer);
    exit(0);
}
