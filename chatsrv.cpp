// Compile the server with the following:
// g++ -o chatsrv chatsrv.cpp -lpthread

// The basic architecture of the server is that it will spawn a thread for each client that connects.
// It will have a global list of the connected clients, and each thread will relay server messages to that list.
// The list will be protected from simultaneous modifications by means of a mutex.
// The main loop for eeach thread will first check to see if the client has sent a command
// If it has, it is dealt with and a return code is sent.
// Then the thread checks to see if there are any pending messages to be sent to the server.
// This loop continues until the coient disconnects or the user has been kicked out of the room.

// You can demo the server behavior by running the executable
// and using netcat to connect and issue commands.

// netcat 127.0.0.1 5296

// JOIN <username>
// MSG <message>
// PMSG <target> <message>
// OP <target>
// TOPIC <new_room_topic>
// KICK <target>
// QUIT

#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <cstring>
#include <unistd.h>

#define LISTEN_PORT 5296
#define MAX_LINE_BUFF 1024

// Here we define two structures
// The first will help us keep track of each connected client
// We have two flags: one to signify whether the user is a room operator,
// and the other to signify that the user was kicked out of the room.
// Finally, we have a vector of strings. This will contain a list of messages that are being relayed to the client

struct client_t {
    bool opstatus;
    bool kickflag;
    std::vector<std::string> outbound;
};

// The second structure is for holding the client commands.
// We will parse the command into this structure for easier handling.
// The maximum number of operands for any command is two.
struct cmd_t
{
    std::string command;
    std::string op1;
    std::string op2;
};

// Globals
// Mutex to synchronize access to the room topic
pthread_mutex_t  room_topic_mutex;
std::string room_topic;
pthread_mutex_t client_list_mutex;
std::map<std::string, client_t> client_list;


void* thread_proc(void *arg);
int readLine(int sock, char *buffer, int buffsize);
cmd_t decodeCommand(const char *buffer);
int join_command(const cmd_t &cmd, std::string &msg);
int msg_command(const cmd_t &cmd, const std::string &nickname, std::string &msg);
int pmsg_command(const cmd_t &cmd, const std::string &nickname, std::string &msg);
int op_command(const cmd_t &cmd, const std::string &nickname, std::string &msg);
int kick_command(const cmd_t &cmd, const std::string &nickname, std::string &msg);
int topic_command(const cmd_t &cmd, const std::string &nickname, std::string &msg);
int quit_command(const std::string &nickname, std::string &msg);

int main(int argc, char *argv[])
{
    struct sockaddr_in sAddr;
    int listensock;
    int newsock;
    int result;
    pthread_t thread_id;
    int flag = 1;

	// Create our listening socket
	// Set the socket option SO_REUSEADDR
	// This will keep us from getting "address in use" errors when stopping & starting the server
    listensock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    setsockopt(listensock, IPPROTO_TCP, SO_REUSEADDR, (char* ) &flag, sizeof(int));

	// Bind our socket to all avaliable addresses & our port
    sAddr.sin_family  = AF_INET;
    sAddr.sin_port = htons(LISTEN_PORT);
    sAddr.sin_addr.s_addr = INADDR_ANY;
    result = bind(listensock, (struct sockaddr *) &sAddr, sizeof(sAddr));

    if (result < 0) {
        perror("chatsrv");
        return 0;
    }

	// Set the socket to listen
    result = listen(listensock, 5);
    if (result < 0) {
        perror("chatsrv");
        return 0;
    }

	// If everything has suceeded so far, we go ahead and initialize our mutexes.
	// This must be done only once and before **any** of the threads use them.

    pthread_mutex_init(&room_topic_mutex, NULL);
    pthread_mutex_init(&client_list_mutex, NULL);

	// Main Loop: We accept incoming connections and start a new thread for each client.
    while (1)
    {
        newsock = accept(listensock, NULL, NULL);
        result = pthread_create(&thread_id, NULL, thread_proc, (void *) newsock);

        if (result != 0) {
            printf("could not create thread.\n");
            return 0;
        }
        pthread_detach(thread_id);
        sched_yield();
    }
    return 1;
}

// Here defines our thread procedure
void* thread_proc(void *arg)
{
    int sock;
    char buffer[MAX_LINE_BUFF];
    int nread;
    int flag = 1;
    bool joined = false;
    bool quit = false;
    std::string nickname;
    struct cmd_t cmd;
    std::string return_msg;
    std::map<std::string, client_t>::iterator client_iter;
    int status;
    std::string outstring;

    // Retrieve socket passed to thread_proc
    sock = (int64_t) arg;
    // Turn off Nagle's algorithm
	// It attempts to group undersized outgoing packets in order to send them out at once
	// This is enabled by default in linux.
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));
    // Set socket to nonblocking
    ioctl(sock, FIONBIO, (char *) &flag);

    while(!quit)
    {
        // Get any commands that the client has sent
        status = readLine(sock, buffer, MAX_LINE_BUFF);

        if (status < 0)
        {
            // If we've lost the client then process it as a QUIT
            if (joined) {
                quit_command(nickname, return_msg);
            }
            return arg;
        } else if (status > 0) {
            cmd = decodeCommand(buffer);
			// If client issues command without joining the room.
            if (!joined && cmd.command != "JOIN") {
                return_msg = "203 DENIED - MUST JOIN FIRST";
            } else {
                if (cmd.command == "JOIN") {
					// Don't let them join twice
                    if (joined) {
                        return_msg = "203 DENIED - ALREADY JOINED";
                    } else {
                        status = join_command(cmd, return_msg);
                        if (status > 0) {
                            joined = true;
                            nickname = cmd.op1;
                        }
                    }
                } else if (cmd.command == "MSG") {
                    msg_command(cmd, nickname, return_msg);
                } else if (cmd.command == "PMSG") {
                    pmsg_command(cmd, nickname, return_msg);
                } else if (cmd.command == "OP") {
                    op_command(cmd, nickname, return_msg);
                } else if (cmd.command == "KICK") {
                    kick_command(cmd, nickname, return_msg);
                } else if (cmd.command == "TOPIC") {
                    topic_command(cmd, nickname, return_msg);
                } else if (cmd.command == "QUIT") {
                    quit_command(nickname, return_msg);
                    joined = false;
                    quit = true;
                } else {
                    return_msg = "900 UNKNOWN COMMAND";
                }
            }
            return_msg += "\n";
            send(sock, return_msg.c_str(), return_msg.length(), 0);
        }

        // Send any pending server commands to the client
        if (joined)
        {
            pthread_mutex_lock(&client_list_mutex);
            for (int i = 0; i < client_list[nickname].outbound.size(); i++) {
                outstring = client_list[nickname].outbound[i] + "\n";
                send(sock, outstring.c_str(), outstring.length(), 0);
            }
            client_list[nickname].outbound.clear();

            if (client_list[nickname].kickflag == true) {
                client_iter = client_list.find(nickname);
                if (client_iter != client_list.end()) {
                    client_list.erase(client_iter);
                }
                quit = true;
            }
            pthread_mutex_unlock(&client_list_mutex);
        }
        sched_yield();
    }
    close(sock);
    return arg;
}


// Commands from the client are terminated with a newline character.
// The function will read up to a newline and then return.
int readLine(int sock, char *buffer, int buffsize)
{
    char *buffPos;
    char *buffEnd;
    int nChars;
    int readlen;
    bool complete = false;
    fd_set fset;
    struct timeval tv;
    int sockStatus;
    int readSize;
    unsigned long blockMode;

	// Is there anything to read?
    FD_ZERO(&fset);
    FD_SET(sock, &fset);
    tv.tv_sec = 0;
    tv.tv_usec = 50;
    sockStatus = select(sock + 1, &fset, NULL, &fset, &tv);
    if (sockStatus <= 0) {
        return sockStatus;
    }

	// Initialize buffer and set pointers for buffPos, and buffEnd
    buffer[0] = '\0';
    buffPos = buffer;
    buffEnd = buffer+buffsize;
    readlen = 0;

	// Reads in data in a loop.
    while (!complete) {
		// Make sure we have buffer room first
        if ((buffEnd-buffPos) < 0) {
            readSize = 0;
        } else {
            readSize = 1;
        }

		// Now we call select() to make sure there is something to read.
		// This also allows us to escape if the client suddenly disconnects or if the client is sending data too slowly.
		// We set a timeout of 5 secs for getting data from the client.
        FD_ZERO(&fset);
        FD_SET(sock, &fset);
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        sockStatus = select(sock + 1, &fset, NULL, &fset,&tv);
        if (sockStatus < 0) {return -1;}

		// Recieve character and check if it's a newline.
		// If so, we are done.
        nChars = recv(sock, (char *) buffPos, readSize, MSG_NOSIGNAL);
        readlen += nChars;
        if (nChars <= 0) {
            return -1;
        }
        if (buffPos[nChars-1]=='\n') {
            complete = true;
            buffPos[nChars - 1] = '\0';
        }
		// Otherwise, we can add it to the buffer and loop.
        buffPos += nChars;
    }
	// returns the size of the string we need.
    return readlen;
}


// Decodes a string received from the client into our cmd_t structure.
// It basically separates the buffer based on the space being the delimiter.
// Since we can have at most two operands for a command, after it finds the second space it places the remaining text from the buffer into second op.
// This works perfectly for our commands such as PMSG, where the second operand can contain spaces.
// It works well enough for the MSG command, which has only one operand.
// If the message contains spaces we just concatenate the two operands, in the msg_command() func.

// NOTE: This is just how I copied from the book, we should definitely do variadic args later
cmd_t decodeCommand(const char *buffer)
{
    struct cmd_t ret_cmd;
    int state;
    state = 0;
    for (int x = 0; x < strlen(buffer); x++) {
        if (buffer[x] == ' ' && state < 2) {
            state++;
        } else {
            switch(state) {
                case 0: ret_cmd.command += toupper(buffer[x]);
                break;
                case 1: ret_cmd.op1 += buffer[x];
                break;
                default: ret_cmd.op2 += buffer[x];
            }
        }
    }
    return ret_cmd;
}

// This function handles the JOIN command and, if successful, adds the user to the client_list map.
int join_command(const cmd_t &cmd, std::string &msg)
{
    int retval;
    std::map<std::string, client_t>::iterator client_iter;

	// Preliminary check on the nickname.
	// If the nickname has a space in it, then it'll have been separated into op1 and op2.
	// Therefore, if op2 is not an empty strin, then the nickname is invalid.
    if(cmd.op1.length()==0 || cmd.op2.length() > 0)
    {
        msg = "201 INVALID NICKNAME";
        return 0;
    } else {
		// Next, we check to make sure that a room member isn't already using the nickname.
		// Before checking the client_list map, we must lock the mutex.
        pthread_mutex_lock(&client_list_mutex);
        client_iter = client_list.find(cmd.op1);

		// If the nickname isn't already in use, we add the client to the room.
		// First, we check to see if this is the first user in the room.
		// If so, then that user automatically becomes a room operator.
		// Notice that we're implicitly adding the user to the room
		// by taking advantage of the fact that an STL map will create a new entry for us if one doesn't exist.
        if (client_iter == client_list.end()) {
            if (client_list.size() == 0) {
                client_list[cmd.op1].opstatus = true;
            } else {
                client_list[cmd.op1].opstatus = false;
            }
            client_list[cmd.op1].kickflag = false;

			// Now, we cycle through the other users in the room, if there are any.
			// We do three things here:
			// First, we tell the others that a new user has joined the room.
			// Second, we gather the names of the users already in the room to relay to the new user.
			// Third, we tell the new user which existing users are room operators.
			// Notice that the server commands are added to the clients' outbound message queue.
			// They will then be sent to the client in the main loop.

            for (client_iter = client_list.begin();
                client_iter != client_list.end(); ++client_iter) {
                // Tell other clients that a new user has joined
                if ((*client_iter).first != cmd.op1) {
                    (*client_iter).second.outbound.push_back("JOIN " +cmd.op1);
                }
                // Tell the new client which users are already in the room
                client_list[cmd.op1].outbound.push_back("JOIN "+(*client_iter).first);

                // Tell the new client who has operator status
                if ((*client_iter).second.opstatus == true) {
                    client_list[cmd.op1].outbound.push_back("OP "+(*client_iter).first);
                }
            }

            // Tell the new client the room topic, then return a success code
            pthread_mutex_lock(&room_topic_mutex);
            client_list[cmd.op1].outbound.push_back("TOPIC * "+room_topic);
            pthread_mutex_unlock(&room_topic_mutex);
            msg = "100 OK";
            retval = 1;
		// If the nickname is in use, then we send a failure notice.
        } else {
            msg = "200 NICKNAME IN USE";
            retval = 0;
        }
        pthread_mutex_unlock(&client_list_mutex);
    }
    return retval;
}

// Next, we handle the MSG command
// This is a simple function, as all it does is relay
// the message to all connected clients by adding it to their outbound queues
int msg_command(const cmd_t &cmd, const std::string &nickname, std::string &msg)
{
    std::map<std::string, client_t>::iterator client_iter;
    pthread_mutex_lock(&client_list_mutex);
    for (client_iter = client_list.begin();
        client_iter != client_list.end(); client_iter++) {
        (*client_iter).second.outbound.push_back("MSG "+ nickname + " " + cmd.op1 + " " + cmd.op2);
    }
    pthread_mutex_unlock(&client_list_mutex);
    msg = "100 OK";

    return 1;
}

// Handling a private message is almost as easy
// First we make sure the recipient exists
// if so, add the message to their outbound queue.
int pmsg_command(const cmd_t &cmd, const std::string &nickname, std::string &msg)
{
    std::map<std::string, client_t>::iterator client_iter;

    pthread_mutex_lock(&client_list_mutex);
    client_iter = client_list.find(cmd.op1);

    if (client_iter == client_list.end()) {
        msg = "202 UNKNOWN NICKNAME";
    } else {
        (*client_iter).second.outbound.push_back("PMSG "+ nickname + " " + cmd.op2);
        msg = "100 OK";
    }
    pthread_mutex_unlock(&client_list_mutex);
    return 1;
}

int op_command(const cmd_t &cmd, const std::string &nickname, std::string &msg)
{
    std::map<std::string, client_t>::iterator client_iter;

    pthread_mutex_lock(&client_list_mutex);
    client_iter = client_list.find(nickname);
	// We get the first client list entry for the user issuing the OP command.
	// If we can't find that entry, then something is wrong.
    if (client_iter == client_list.end()) {
        msg = "999 UNKNOWN";
    } else {
		// Next we verify that the caller is a room operator.
        if ((*client_iter).second.opstatus == false) {
            msg = "203 DENIED";
        } else {
			// If the verification succeeds, we then check to make sure
			// that the target of the OP command is valid.
            client_iter = client_list.find(cmd.op1);
            if (client_iter == client_list.end()) {
                msg = "202 UNKNOWN NICKNAME";
            } else {
				// Finally, we set the operator status flag on the recipient
				// and notify the room members of the new room operator.
                (*client_iter).second.opstatus = true;
                for (client_iter = client_list.begin();
                    client_iter != client_list.end(); client_iter++) {
                    (*client_iter).second.outbound.push_back("OP " + cmd.op1);
                }
                msg = "100 OK";
            }
        }
    }
    pthread_mutex_unlock(&client_list_mutex);
    return 1;
}

// The KICK command causes a user to be forcibly removed from the room.
int kick_command(const cmd_t &cmd, const std::string &nickname, std::string &msg)
{
    std::map<std::string, client_t>::iterator client_iter;
	// We first get the client list entry for the user issuing the command.
	// If we can't find that entry, then something is wrong.
    pthread_mutex_lock(&client_list_mutex);
    client_iter = client_list.find(nickname);
    if (client_iter == client_list.end()) {
        msg = "999 UNKNOWN";
    } else {
		// Next, we verify that the caller is a room operator.
        if ((*client_iter).second.opstatus == false) {
            msg = "203 DENIED";
        } else {
			// Then, we validate the target of the KICK command

            client_iter = client_list.find(cmd.op1);
            if (client_iter == client_list.end()) {
                msg = "202 UNKNOWN NICKNAME";
            } else {
				// Finally, we set the kick flag on the user being removed,
				// and notify everyone in the room about what happened
				// the kick flag will be picked up in the main loop
				// and the client will be disconnected
                (*client_iter).second.kickflag = true;
                for (client_iter = client_list.begin();
                    client_iter != client_list.end(); client_iter++) {
                    (*client_iter).second.outbound.push_back("KICK "+cmd.op1 + " " + nickname);
                }
                msg = "100 OK";
            }
        }
    }
    pthread_mutex_unlock(&client_list_mutex);
    return 1;
}

// This function handles the TOPIC command
// Room operators have the ability to set a topic for the room.
// It is very similar to the other "operator-only" commands.
int topic_command(const cmd_t &cmd, const std::string &nickname, std::string &msg) {
    std::map<std::string, client_t>::iterator client_iter;

    pthread_mutex_lock(&client_list_mutex);
    client_iter = client_list.find(nickname);
    if (client_iter == client_list.end()) {
        msg == "999 UNKNOWN";
    } else {
        if ((*client_iter).second.opstatus == false) {
            msg = "203 DENIED";
        } else {
            pthread_mutex_lock(&room_topic_mutex);
            room_topic = cmd.op1;
            if (cmd.op2.length() != 0) {
                room_topic += " " + cmd.op2;
            }
            for (client_iter = client_list.begin();
                 client_iter != client_list.end(); client_iter++) {
                (*client_iter).second.outbound.push_back("TOPIC " + nickname + " " + room_topic);
            }
            pthread_mutex_lock(&room_topic_mutex);
            msg = "100 OK";
        }
    }
    pthread_mutex_unlock(&client_list_mutex);
    return 1;
}

// This function handles the QUIT command
// We remove the client from the client_list map and notify the others that the client has left.
int quit_command(const std::string &nickname, std::string &msg)
{
    std::map<std::string, client_t>::iterator client_iter;

    pthread_mutex_lock(&client_list_mutex);
    client_iter = client_list.find(nickname);
    if (client_iter == client_list.end()) {
        msg = "999 UNKNOWN";
    } else {
        client_list.erase(client_iter);
        for (client_iter = client_list.begin(); client_iter != client_list.end(); client_iter++)
        {
            (*client_iter).second.outbound.push_back("QUIT "+nickname);
        }
        msg = "100 OK";
    }
    pthread_mutex_unlock(&client_list_mutex);
}
