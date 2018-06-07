#include <iostream>
#include <signal.h>
#include <unistd.h>
#include <microhttpd.h>
#include <cstdlib>
#include <queue>
#include <cstring>
#include <mutex>
#include <thread>
#include <sstream>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include "json.hpp"

using namespace std;

static const char alphanum[] =
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz";

const char VER[] = "ver. 1";
const char ADD[] = "/v1/add";
const char STAT[] = "/v1/stat";
const int KEY_LEN = 10;

volatile sig_atomic_t stop;

uint16_t balancer = 0;

enum  portStatus {unused, active, inactive};


struct ssPort
{
    uint16_t portNum;
    portStatus stat;
    string key;
    ssPort()
    {
        stat = unused;
    }
};

mutex collisionPrev;

struct connectionInfo
{
    vector<char> data;
};

string genPass()
{
    string result;
    for (int i = 0; i < KEY_LEN; ++i)
        result += (alphanum[rand() % (sizeof(alphanum) - 1)]);

    return result;
}

void inthand(int signum)
{
    stop = 1;
}

void cleaner(uint32_t timeout, queue<ssPort>* scedRemQ, vector<ssPort>* ssPorts, deque<uint16_t>* availPrts)
{
    uint32_t counter = 0;
    while(!stop)
        if(counter >= timeout)
        {
            collisionPrev.lock();
                for (ssPort &p : *ssPorts)
                {
                    switch (p.stat)
                    {
                    case inactive:
                        p.stat = unused;
                        scedRemQ->push(p);
                        availPrts->push_back(p.portNum);
                    break;
                    case active:
                        p.stat = inactive;
                    break;
                    case unused:
                    break;
                    }
                }
            collisionPrev.unlock();

            counter = 0;
        }
        else
        {
            sleep(1);
            counter++;
        }
}

int codeResponce(MHD_Connection *connection, unsigned int status_code)
{
    MHD_Response *response = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
    int ret = MHD_queue_response(connection, status_code, response);
    MHD_destroy_response(response);
    return ret;
}

static int respondRequest(MHD_Connection *connection, void* params)
{
    tuple<deque<uint16_t>*, queue<ssPort>*, vector<ssPort>*>* parameters = static_cast<tuple<deque<uint16_t>*, queue<ssPort>*, vector<ssPort>*>*>(params);
    deque<uint16_t>* availablePorts = get<0>(*parameters);
    queue<ssPort>* addQueue         = get<1>(*parameters);
    vector<ssPort>* serverPorts     = get<2>(*parameters);

    const uint16_t minPort = serverPorts->at(0).portNum;
    bool overloaded = availablePorts->empty();

    nlohmann::json jObj;
    ssPort newPort;

    if(overloaded)
    {
        balancer = (balancer==serverPorts->size()-1)?0:++balancer;
        newPort.portNum = serverPorts->at(balancer).portNum;
        newPort.key = serverPorts->at(balancer).key;
    }
    else
    {
        string sessionKey = genPass();
        collisionPrev.lock();
            uint16_t nPN = availablePorts->front();
            availablePorts->pop_front();

            newPort.portNum = nPN;
            newPort.key = sessionKey;

            serverPorts->at(nPN-minPort).key = sessionKey;
            serverPorts->at(nPN-minPort).stat = active;

            addQueue->push(newPort);
        collisionPrev.unlock();
    }

    jObj["port"] = newPort.portNum;
    jObj["key"] = newPort.key;

    std::stringstream buffer;
    buffer << jObj.dump();
    string reply = buffer.str();

    cout << reply << endl;

    int sz = reply.length();
    void* m = malloc(sz);
    memcpy(m, reply.c_str(), sz);

    MHD_Response *response = MHD_create_response_from_buffer(sz, m, MHD_RESPMEM_MUST_FREE);
    int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return ret;
}

int getStat(MHD_Connection *connection, void* params)
{
    tuple<deque<uint16_t>*, queue<ssPort>*, vector<ssPort>*>* parameters = static_cast<tuple<deque<uint16_t>*, queue<ssPort>*, vector<ssPort>*>*>(params);
    deque<uint16_t>* availablePorts = get<0>(*parameters);
    vector<ssPort>* serverPorts     = get<2>(*parameters);

    int totalPortNum = serverPorts->size();
    int freePortNum = availablePorts->size();

    string util = to_string(int((totalPortNum-freePortNum)*100/totalPortNum));
    util.append(" %");

    int sz = util.length();
    void* m = malloc(sz);
    memcpy(m, util.c_str(), sz);

    MHD_Response *response = MHD_create_response_from_buffer(sz, m, MHD_RESPMEM_MUST_FREE);
    int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return ret;
}

static int answer_to_connection (void *cls, struct MHD_Connection *connection,
                      const char *url, const char *method,
                      const char *version, const char *upload_data,
                      size_t *upload_data_size, void **con_cls)
{
/*
    if (*con_cls == NULL)
    {
        *con_cls = new connectionInfo();
        return MHD_YES;
    }

    connectionInfo* conInfo = static_cast<connectionInfo*>(*con_cls);

    if (*upload_data_size != 0)
    {
        conInfo->data.insert(conInfo->data.end(), upload_data, upload_data+*upload_data_size);

        *upload_data_size = 0;

        return MHD_YES;
    }

    return respondRequest(connection, conInfo, cls);*/

    if(!strcmp(method, MHD_HTTP_METHOD_GET) == 0)
        return codeResponce(connection, MHD_HTTP_METHOD_NOT_ALLOWED);

    if(strcmp(url, STAT) == 0)
        return getStat(connection, cls);

    if(strcmp(url, ADD) == 0)
        return respondRequest(connection, cls);

    return codeResponce(connection, MHD_HTTP_NOT_FOUND);
}

static void request_completed (void *cls, struct MHD_Connection *connection,
                   void **con_cls, enum MHD_RequestTerminationCode toe)
{
    connectionInfo* conInfo = static_cast<connectionInfo*>(*con_cls);

    delete conInfo;
    *con_cls = NULL;
}

void statParser(int socket_fd, vector<ssPort>* ssPorts)
{
    const char delim = ':';
    const char mark = '"';

    const uint16_t minPort = ssPorts->at(0).portNum;
    const uint16_t portCount = ssPorts->size();

    vector<bool> activePorts(portCount);
    char buf[1024*100];

    int n = 0;

    while(!stop)
    {
        memset(buf, 0, sizeof(buf));
        fill(activePorts.begin(), activePorts.end(), false);

        //GetSock from stat
        n = recv(socket_fd, buf, sizeof(buf), 0);
        if (n <= 0)
            continue;

        cout << buf << endl;

        //Parse
        istringstream iss(string(buf, n));
        string item;
        while (getline(iss, item, delim))
            if(item.back()!=mark)
                continue;
            else
            {
                size_t pos = item.find(mark);
                if(pos == item.length()-1)
                    continue;
                else
                {
                    uint16_t activePort = 0;
                    try
                    {
                        string sub = item.substr(pos+1, item.length()-pos-2);
                        activePort = stoi(sub);
                        activePorts[activePort-minPort] = true;
                    }
                    catch(...)
                    {
                        continue;
                    }
                }
            }

        //Update
        collisionPrev.lock();
            for(int i=0; i<portCount; i++)
                if(activePorts[i])
                    ssPorts->at(i).stat = active;
        collisionPrev.unlock();
    }
}

int socketInit(char* serverManagerSocket)
{
    const char* server_filename = serverManagerSocket;
    const char* client_filename = "./ssMan.sock";

    struct sockaddr_un server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, server_filename, strlen(server_filename));

    struct sockaddr_un client_addr;
    memset(&client_addr, 0, sizeof(client_addr));
    client_addr.sun_family = AF_UNIX;
    strncpy(client_addr.sun_path, client_filename, strlen(client_filename));
    client_addr.sun_path[0] = '\0';

    int sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sockfd < 0)
        return -1;

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if(setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) < 0)
        return -2;

    unlink(client_filename);

    if(bind(sockfd, (struct sockaddr *) &client_addr, sizeof(client_addr)) < 0)
        return -3;

    if(connect(sockfd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0)
        return -4;

    return sockfd;
}

int main(int argc, char* argv[])
{
    cout << VER << endl;
    signal(SIGINT, inthand);
    srand (time(NULL));

   if(argc!=4)
   {
       cout << "usage:" << endl;
       cout << argv[0] << " <ssserver manager socket> <min port> <max port>" << endl;
       cout << "example:" << endl;
       cout << argv[0] << " /tmp/manSock.sock 8380 8390" << endl;
       cout << "local REST port: 8888" << endl;
       return 0;
   }

    char* server_filename = argv[1];
    uint16_t portMin = atoi(argv[2]);
    uint16_t portMax = atoi(argv[3]);


    deque<uint16_t> availablePorts;
    for (uint16_t p = portMin; p < portMax; p++)
        availablePorts.push_back(p);


    const int16_t pn = availablePorts.size();
    if(pn ==0 )
        return -5;

    vector<ssPort> serverPorts(pn);
    for (uint16_t p = 0; p < pn; p++)
        serverPorts.at(p).portNum = portMin + p;


    queue<ssPort> addQueue;
    queue<ssPort> remQueue;

    int serverManagerSock = socketInit(server_filename);
    if(serverManagerSock < 0)
        return serverManagerSock;

    tuple<deque<uint16_t>*, queue<ssPort>*, vector<ssPort>*> params = make_tuple(&availablePorts, &addQueue, &serverPorts);

    struct MHD_Daemon *restDaemon;

    restDaemon = MHD_start_daemon (MHD_USE_SELECT_INTERNALLY, 8888, NULL, NULL,
                                   &answer_to_connection, &params,
                                   MHD_OPTION_END);
    if (NULL == restDaemon)
        return -6;

    thread vanisher(cleaner, 60, &remQueue, &serverPorts, &availablePorts);
    thread activator(statParser, dup(serverManagerSock), &serverPorts);

    const char handshake[] = "ping";
    send(serverManagerSock, handshake, strlen(handshake), 0);

    char commandBuf[100];

    while (!stop)
    {
        bool insomnia = false;

        if(!remQueue.empty())
        {
            collisionPrev.lock();
            while(!remQueue.empty())
            {
                memset(commandBuf, 0, sizeof(commandBuf));
                sprintf(commandBuf, "remove: {\"server_port\": %d}", remQueue.front().portNum);
                cout << commandBuf << endl;
                send(serverManagerSock, commandBuf, strlen(commandBuf), 0);
                remQueue.pop();
            }
            collisionPrev.unlock();

            insomnia = true;
        }

        if(!addQueue.empty())
        {
            collisionPrev.lock();
            while(!addQueue.empty())
            {
                memset(commandBuf, 0, sizeof(commandBuf));
                sprintf(commandBuf, "add: {\"server_port\": %d, \"password\":\"%s\"}", addQueue.front().portNum, addQueue.front().key.c_str());
                cout << commandBuf << endl;
                send(serverManagerSock, commandBuf, strlen(commandBuf), 0);
                addQueue.pop();
            }
            collisionPrev.unlock();

            insomnia = true;
        }


        if(!insomnia)
            usleep(100000);
    }

    MHD_stop_daemon (restDaemon);
    activator.join();
    vanisher.join();
    close(serverManagerSock);


    return 0;
}
