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

using namespace std;

const int PASS_LEN = 10;

volatile sig_atomic_t stop;

uint16_t balancer = 0;

enum  portStatus {unused, active, inactive};

struct ssPort
{
    uint16_t portNum;
    portStatus stat;
    char key[PASS_LEN];
    ssPort()
    {
        stat = unused;
    }
};

mutex collisionPrev;

void genPass(char *s, const int len)
{
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    for (int i = 0; i < len; ++i) {
        s[i] = alphanum[rand() % (sizeof(alphanum) - 1)];
    }
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

static int answer_to_connection (void *cls, struct MHD_Connection *connection,
                      const char *url, const char *method,
                      const char *version, const char *upload_data,
                      size_t *upload_data_size, void **con_cls)
{
  struct MHD_Response *response;
  int ret;

  response = MHD_create_response_from_buffer (0, NULL, MHD_RESPMEM_PERSISTENT);
  ret = MHD_queue_response (connection, MHD_HTTP_OK, response );
  MHD_destroy_response (response);

  return ret;
}

void statParser(int socket_fd, vector<ssPort>* ssPorts)
{
    const char delim = ':';
    const char mark = '"';

    const uint16_t minPort = ssPorts->at(0).portNum;
    const uint16_t portCount = ssPorts->size();

    vector<bool> activePorts(portCount);
    char buf[1024*100];


    while(!stop)
    {
        memset(buf, 0, sizeof(buf));
        fill(activePorts.begin(), activePorts.end(), false);
        //GetSock from stat

        //Parse
        istringstream iss(string(buf, sizeof(buf)));
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

int main(int argc, char* argv[])
{
    signal(SIGINT, inthand);

    uint16_t portMin = 10, portMax = 20;

    deque<uint16_t> availablePorts;
    for (uint16_t p = portMin; p < portMax; p++)
        availablePorts.push_back(p);


    const int16_t pn = availablePorts.size();
    if(pn <=0 )
        return 0;

    vector<ssPort> serverPorts(pn);
    for (uint16_t p = 0; p < pn; p++)
        serverPorts.at(p).portNum = portMin + p;

    //Connect to socket

    queue<ssPort> addQueue;
    queue<ssPort> remQueue;

    thread vanisher(cleaner, 1000, &remQueue, &serverPorts, &availablePorts);
    thread activator(statParser, dup(1), &serverPorts);

    struct MHD_Daemon *restDaemon;

    restDaemon = MHD_start_daemon (MHD_USE_SELECT_INTERNALLY, 8888, NULL, NULL, &answer_to_connection, NULL, MHD_OPTION_END);
    if (NULL == restDaemon)
        return 0;

    while (!stop)
    {
        bool insomnia = false;

        if(!addQueue.empty())
        {
            collisionPrev.lock();
            //add
            while(!addQueue.empty())
                addQueue.pop();
            collisionPrev.unlock();

            insomnia = true;
        }

        if(!remQueue.empty())
        {
            collisionPrev.lock();
            //remove
            while(!remQueue.empty())
                remQueue.pop();
            collisionPrev.unlock();

            insomnia = true;
        }

        if(!insomnia)
            usleep(100000);
    }

    MHD_stop_daemon (restDaemon);

    vanisher.join();
    activator.join();

    return 0;
}
