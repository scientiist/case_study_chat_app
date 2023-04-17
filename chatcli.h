#pragma once
#include <string>

#define MAX_LINE_BUFF 1024

// Define our command structure
// This is the same structure that the server used

struct cmd_t  {
	std::string command;
	std::string op1;
	std::string op2;
};

// Function declarations
int connectAndJoin(std::string host, int port, std::string nickname);
int readLine(int sock, char *buffer, int buffsize;
cmd_t decodeCommand(const char *buffer);

