#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cerrno>
#include <stdio.h>
#include <arpa/inet.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sstream>
#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <vector>
#include <fstream>
#include "rapidxml-1.13/rapidxml.hpp"
#include "rapidxml-1.13/rapidxml_print.hpp"
#include <algorithm>

#define PORT 2000

std::string time_plus_delay(const std::string &time, int delay)
{
    std::tm t = {};
    std::stringstream ss(time);
    ss >> std::get_time(&t, "%H:%M");

    std::time_t tt = std::mktime(&t);
    tt += delay * 60;
    std::tm *updated_tm = std::localtime(&tt);

    std::stringstream arrival_delay;
    arrival_delay << std::put_time(updated_tm, "%H:%M");

    return arrival_delay.str();
}

std::string get_current_day() // e.g. Monday
{
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm *now_tm = std::localtime(&now_time);

    std::stringstream ss;
    ss << std::put_time(now_tm, "%A");
    std::string day = ss.str();

    return day;
}

std::string get_current_time() // hh:mm
{
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm *now_tm = std::localtime(&now_time);

    std::stringstream ss;
    ss << std::put_time(now_tm, "%H:%M");
    std::string time = ss.str();

    return time;
}

bool is_within_one_hour(const std::string time_string) // to check if there are arrivals/departures in the next hour
{
    std::string current_time_string = get_current_time();

    std::tm curr_time_str = {};
    std::tm time_str = {};

    std::istringstream time(time_string);
    time >> std::get_time(&time_str, "%H:%M");

    std::istringstream current(current_time_string);
    current >> std::get_time(&curr_time_str, "%H:%M");

    std::time_t current_time = mktime(&curr_time_str);
    std::time_t other_time = mktime(&time_str);

    double diff = std::difftime(other_time, current_time);
    return diff >= 0 && diff <= 3600;
}

struct Station
{
    std::string name;
    std::string arrival;
    std::string departure;
    std::string delay;
    std::string depdelay;
};
struct Train
{
    std::string id;
    std::string day;
    std::vector<Station> stations;
};

struct Schedule
{
    std::vector<Train> trains;
} schedule;

std::vector<std::string> cities;

pthread_mutex_t updatelock = PTHREAD_MUTEX_INITIALIZER;
std::vector<std::string> updates;

// convert adress to ip:port pair
char *conv_addr(struct sockaddr_in address)
{
    static char str[25];
    char port[7];

    strcpy(str, inet_ntoa(address.sin_addr)); // IP ADDR
    bzero(port, 7);                           // PORT
    sprintf(port, ":%d", ntohs(address.sin_port));
    strcat(str, port);
    return (str);
}

typedef struct
{
    pthread_t t_id;
    int t_count;
} Thread;

Thread *threadpool;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
int sv_shutdown = 0;
int sd, num_threads;

void help_str(char *help)
{
    strcpy(help, "Connected to server. Available commands:\n");
    strcat(help, "1. schedule -> shows train schedule for today\n");
    strcat(help, "2. signal <arrival/departure> <train_id> <city> <minutes> -> signal a train being late (max 60 min at a time)\n");
    strcat(help, "3. reset <train_id> -> reset a train's delays\n");
    strcat(help, "4. nexthour <city> -> shows arrivals/departures in/from <city> in the next hour\n");
    strcat(help, "5. trains <city> -> shows all trains passing through a city (city is case sensitive)\n");
    strcat(help, "6. cities -> list of available cities\n");
    strcat(help, "7. help -> list of available commands\n");
    strcat(help, "8. quit -> close the CLI app\n");
    help[strlen(help)] = '\0';
}

void sighandler(int sig) // ctrl c handler for graceful shutdown
{
    sv_shutdown = 1;
    printf("\n[server] cleaning up...\n");
    fflush(stdout);
}

int check_cmd(char *command) // validate command and args
{
    char cmd[2048];
    strcpy(cmd, command);

    char *token = strtok(cmd, " ");
    if (strcmp(token, "quit") == 0)
    {
        token = strtok(NULL, " ");
        if (token != NULL)
            return -2; // too many args
        return 1;
    }

    if (strcmp(token, "help") == 0)
    {
        token = strtok(NULL, " ");
        if (token != NULL)
            return -2; // too many args
        return 2;
    }

    if (strcmp(token, "cities") == 0)
    {
        token = strtok(NULL, " ");
        if (token != NULL)
            return -2; // too many args
        return 3;
    }

    if (strcmp(token, "schedule") == 0)
    {
        token = strtok(NULL, " ");
        if (token != NULL)
            return -2; // too many args
        return 4;
    }

    if (strcmp(token, "trains") == 0)
    {
        token = strtok(NULL, " ");
        if (token == NULL)
            return -1; // missing arguments
        // check city
        bool found = false;
        for (const auto &city : cities)
        {
            if (city == token)
                found = true;
        }
        token = strtok(NULL, " ");
        if (token != NULL)
            return -2; // too many args
        if (found)
            return 5; // valid
        return 8;     // invalid
    }

    if (strcmp(token, "nexthour") == 0)
    {
        token = strtok(NULL, " ");
        if (token == NULL)
            return -1; // missing arguments
        // check city
        bool found = false;
        for (const auto &city : cities)
        {
            if (city == token)
                found = true;
        }
        token = strtok(NULL, " ");
        if (token != NULL)
            return -2; // too many args
        if (found)
            return 6; // valid
        return 8;     // invalid
    }

    if (strcmp(token, "signal") == 0)
    {
        token = strtok(NULL, " ");
        if (token == NULL)
            return -1; // missing arguments

        if (strcmp(token, "arrival") != 0 && strcmp(token, "departure") != 0)
            return 13; // invalid argument

        token = strtok(NULL, " ");
        if (token == NULL)
            return -1; // missing arguments
        // check train id
        bool found = false;
        for (const auto &train : schedule.trains)
        {
            if (train.id == token)
                found = true;
        }
        if (!found)
            return 9; // invalid train id
        std::string train_id = token;
        // check if train runs today
        std::string today = get_current_day();
        for (const auto &train : schedule.trains)
        {
            if (train.id == train_id)
            {
                if (train.day != today)
                    return 10; // cant signal a train that doesnt run today
            }
        }

        token = strtok(NULL, " ");
        if (token == NULL)
            return -1; // missing arguments
        // check station
        found = false;
        for (const auto &train : schedule.trains)
        {
            if (train.id == train_id)
            {
                for (const auto &station : train.stations)
                {
                    if (station.name == token)
                        found = true;
                }
            }
        }
        if (!found)
            return 8; // invalid station for train ID.

        token = strtok(NULL, " ");
        if (token == NULL)
            return -1; // missing arguments
        // check time
        int minutes = atoi(token);
        if (minutes <= 0 || minutes > 60)
            return 11; // invalid time

        token = strtok(NULL, " ");
        if (token != NULL)
            return -2; // too many args
        return 7;      // valid
    }

    if (strcmp(token, "reset") == 0)
    {
        token = strtok(NULL, " ");
        if (token == NULL)
            return -1; // missing arguments
        // check train id
        bool found = false;
        for (const auto &train : schedule.trains)
        {
            if (train.id == token)
                found = true;
        }
        if (!found)
            return 9; // invalid train id
        std::string train_id = token;
        // check if train runs today
        std::string today = get_current_day();
        for (const auto &train : schedule.trains)
        {
            if (train.id == train_id)
            {
                if (train.day != today)
                    return 10; // cant signal a train that doesnt run today
            }
        }

        token = strtok(NULL, " ");
        if (token != NULL)
            return -2; // too many args
        return 12;     // valid
    }
    return 0;
}

void write_backup_xml()
{
    std::ofstream file("timetable_backup.xml");
    if (!file.is_open())
    {
        std::cerr << "[server] unable to open timetable_backup.xml" << std::endl;
        return;
    }

    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += "<schedule>\n";
    for (const auto &train : schedule.trains)
    {
        xml += "\t<train id=\"";
        xml += train.id;
        xml += "\" day=\"";
        xml += train.day;
        xml += "\">\n";
        for (const auto &station : train.stations)
        {
            xml += "\t\t<station>\n";
            xml += "\t\t\t<name>";
            xml += station.name;
            xml += "</name>\n";
            xml += "\t\t\t<arrival>";
            xml += station.arrival;
            xml += "</arrival>\n";
            xml += "\t\t\t<departure>";
            xml += station.departure;
            xml += "</departure>\n";
            xml += "\t\t\t<delay>";
            xml += station.delay;
            xml += "</delay>\n";
            xml += "\t\t\t<depdelay>";
            xml += station.depdelay;
            xml += "</depdelay>\n";
            xml += "\t\t</station>\n";
        }
        xml += "\t</train>\n";
    }
    xml += "</schedule>\n";

    file << xml;
    file.close();
}

void *client_handler(void *arg)
{
    int idx = *(int *)arg;
    free(arg);

    struct sockaddr_in from;
    bzero(&from, sizeof(from));
    printf("[thread %d] ready.\n", idx);

    while (!sv_shutdown)
    {
        pthread_mutex_lock(&lock);

        if (sv_shutdown)
        {
            pthread_mutex_unlock(&lock);
            break;
        }

        fd_set read_from;
        FD_ZERO(&read_from);
        FD_SET(sd, &read_from);

        // timeout 2 seconds on select to check if we need to shutdown
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        int poll = select(sd + 1, &read_from, NULL, NULL, &tv);

        if (poll < 0)
        {
            printf("[thread %d] error: select() on sd.\n", idx);
            pthread_mutex_unlock(&lock);
            continue;
        }
        else if (poll == 0)
        {
            pthread_mutex_unlock(&lock);
            continue;
        }
        else if (poll > 0 && FD_ISSET(sd, &read_from))
        {
            socklen_t length = sizeof(from);
            int client = accept(sd, (struct sockaddr *)&from, &length);
            pthread_mutex_unlock(&lock);
            if (client < 0)
            {
                if (errno == EBADF)
                    printf("[thread %d] server is shutting down, can't accept() on this socket anymore.\n", idx);
                else
                    printf("[thread %d] error: accept().\n", idx);
                break;
            }
            else if (client >= 0)
            {
                printf("[thread %d] new client from address: %s\n", idx, conv_addr(from));

                // send command list on connect
                char buf[2048];
                bzero(buf, 2048);
                help_str(buf);

                int wbyte;
                if ((wbyte = write(client, buf, strlen(buf) + 1)) < 0)
                {
                    printf("[thread %d] error: write() to client.\n");
                    close(client);
                    continue;
                }

                threadpool[idx].t_count++;
                int client_quit = 0;

                int update_count = updates.size();

                while (!client_quit && !sv_shutdown)
                {
                    FD_ZERO(&read_from);
                    FD_SET(client, &read_from);

                    tv.tv_sec = 0;
                    tv.tv_usec = 0;

                    poll = select(client + 1, &read_from, NULL, NULL, &tv);
                    if (poll < 0)
                    {
                        printf("[thread %d] error: select() on client.\n", idx);
                        pthread_mutex_unlock(&lock);
                        continue;
                    }
                    if (poll > 0 && FD_ISSET(client, &read_from))
                    {
                        char buffer[2048];
                        bzero(buffer, 2048);
                        int bytes, wbytes;

                        bytes = read(client, buffer, 2048);
                        if (bytes <= 0)
                        {
                            if (errno == ECONNRESET || bytes == 0)
                            {
                                printf("[thread %d] client has disconnected.\n", idx);
                                fflush(stdout);
                            }
                            else
                                printf("[thread %d] error: read() from client.\n", idx);
                            close(client);
                            client_quit = 1;
                            continue;
                        }
                        else
                        {
                            printf("[thread %d] client sent the command: %s\n", idx, buffer);
                            // validate command and arguments
                            int is_valid = check_cmd(buffer);
                            char cmd_copy[2048];
                            strcpy(cmd_copy, buffer);
                            bzero(buffer, 2048);

                            if (is_valid == -2)
                            {
                                strcpy(buffer, "Too many arguments. Try \"help\"\n");
                            }

                            if (is_valid == -1)
                            {
                                strcpy(buffer, "Missing argument(s). Try \"help\"\n");
                            }

                            if (is_valid == 0)
                            {
                                strcpy(buffer, "Invalid command. Try \"help\" for a list of available commands.\n");
                            }

                            if (is_valid == 1) // quit
                            {
                                printf("[thread %d] client has disconnected.\n", idx);
                                fflush(stdout);
                                client_quit = 1;
                                close(client);
                                continue;
                            }

                            if (is_valid == 2) // help
                            {
                                help_str(buffer);
                            }

                            if (is_valid == 3) // cities
                            {
                                strcpy(buffer, "Cities:\n");
                                for (const auto &city : cities)
                                {
                                    strcat(buffer, city.c_str());
                                    strcat(buffer, "\n");
                                }
                                buffer[strlen(buffer)] = '\0';
                            }

                            if (is_valid == 4) // schedule
                            {
                                std::string today = get_current_day();
                                strcpy(buffer, "Schedule for ");
                                strcat(buffer, today.c_str());
                                strcat(buffer, "\n");
                                for (const auto &train : schedule.trains)
                                {
                                    if (train.day == today)
                                    {
                                        strcat(buffer, "Train: ");
                                        strcat(buffer, train.id.c_str());
                                        strcat(buffer, "\n");
                                        for (const auto &station : train.stations)
                                        {
                                            strcat(buffer, "\tStation: ");
                                            strcat(buffer, station.name.c_str());
                                            strcat(buffer, "\n");
                                            strcat(buffer, "\t\tArrival: ");
                                            strcat(buffer, station.arrival.c_str());
                                            if (station.delay != "0")
                                            {
                                                strcat(buffer, " (+");
                                                strcat(buffer, station.delay.c_str());
                                                strcat(buffer, " - ");
                                                std::string depd = time_plus_delay(station.arrival, atoi(station.delay.c_str()));
                                                strcat(buffer, depd.c_str());
                                                strcat(buffer, ")");
                                            }
                                            strcat(buffer, "\n");
                                            strcat(buffer, "\t\tDeparture: ");
                                            strcat(buffer, station.departure.c_str());
                                            if (station.depdelay != "0" && station.departure != "None")
                                            {
                                                strcat(buffer, " (+");
                                                strcat(buffer, station.depdelay.c_str());
                                                strcat(buffer, " - ");
                                                std::string depd = time_plus_delay(station.departure, atoi(station.depdelay.c_str()));
                                                strcat(buffer, depd.c_str());
                                                strcat(buffer, ")");
                                            }
                                            strcat(buffer, "\n");
                                        }
                                    }
                                }
                                buffer[strlen(buffer)] = '\0';
                            }

                            if (is_valid == 5) // trains <city>
                            {
                                char *token = strtok(cmd_copy, " ");
                                token = strtok(NULL, " ");
                                std::string city = token;
                                strcpy(buffer, "Trains passing through ");
                                strcat(buffer, city.c_str());
                                strcat(buffer, ":\n");

                                for (const auto &train : schedule.trains)
                                {
                                    for (const auto &station : train.stations)
                                    {
                                        if (station.name == city)
                                        {
                                            strcat(buffer, "Train: ");
                                            strcat(buffer, train.id.c_str());
                                            strcat(buffer, ", Day of the Week: ");
                                            strcat(buffer, train.day.c_str());
                                            strcat(buffer, "\n");
                                            strcat(buffer, "\tFrom: ");
                                            strcat(buffer, train.stations[0].name.c_str());
                                            strcat(buffer, "\n");
                                            strcat(buffer, "\tTo: ");
                                            strcat(buffer, train.stations[train.stations.size() - 1].name.c_str());
                                            strcat(buffer, "\n");
                                            strcat(buffer, "\tStation: ");
                                            strcat(buffer, station.name.c_str());
                                            strcat(buffer, "\n");
                                            strcat(buffer, "\t\tArrival: ");
                                            strcat(buffer, station.arrival.c_str());
                                            if (station.delay != "0")
                                            {
                                                strcat(buffer, " (+");
                                                strcat(buffer, station.delay.c_str());
                                                strcat(buffer, " - ");
                                                std::string depd = time_plus_delay(station.arrival, atoi(station.delay.c_str()));
                                                strcat(buffer, depd.c_str());
                                                strcat(buffer, ")");
                                            }
                                            strcat(buffer, "\n");
                                            strcat(buffer, "\t\tDeparture: ");
                                            strcat(buffer, station.departure.c_str());
                                            if (station.depdelay != "0" && station.departure != "None")
                                            {
                                                strcat(buffer, " (+");
                                                strcat(buffer, station.depdelay.c_str());
                                                strcat(buffer, " - ");
                                                std::string depd = time_plus_delay(station.departure, atoi(station.depdelay.c_str()));
                                                strcat(buffer, depd.c_str());
                                                strcat(buffer, ")");
                                            }
                                            strcat(buffer, "\n");
                                        }
                                    }
                                }
                                buffer[strlen(buffer)] = '\0';
                            }

                            if (is_valid == 6) // nexthour <city> -> shows arrivals/departures in/from <city> in the next hour
                                               // also acounts arrival/departure+delay
                            {
                                char *token = strtok(cmd_copy, " ");
                                token = strtok(NULL, " ");
                                std::string city = token;
                                strcpy(buffer, "Arrivals/Departures in ");
                                strcat(buffer, city.c_str());
                                strcat(buffer, " in the next hour:\n");
                                int count = 0;
                                for (const auto &train : schedule.trains)
                                {
                                    for (const auto &station : train.stations)
                                    {
                                        if (station.name == city && get_current_day() == train.day)
                                        {

                                            if (is_within_one_hour(station.arrival) || is_within_one_hour(station.departure))
                                            {
                                                count++;
                                                strcat(buffer, "Train: ");
                                                strcat(buffer, train.id.c_str());
                                                strcat(buffer, "\n");
                                                strcat(buffer, "\tFrom: ");
                                                strcat(buffer, train.stations[0].name.c_str());
                                                strcat(buffer, "\n");
                                                strcat(buffer, "\tTo: ");
                                                strcat(buffer, train.stations[train.stations.size() - 1].name.c_str());
                                                strcat(buffer, "\n");
                                                strcat(buffer, "\tStation: ");
                                                strcat(buffer, station.name.c_str());
                                                strcat(buffer, "\n");
                                                strcat(buffer, "\t\tArrival: ");
                                                strcat(buffer, station.arrival.c_str());
                                                if (station.delay != "0")
                                                {
                                                    strcat(buffer, " (+");
                                                    strcat(buffer, station.delay.c_str());
                                                    strcat(buffer, " - ");
                                                    std::string depd = time_plus_delay(station.arrival, atoi(station.delay.c_str()));
                                                    strcat(buffer, depd.c_str());
                                                    strcat(buffer, ")");
                                                }
                                                strcat(buffer, "\n");
                                                strcat(buffer, "\t\tDeparture: ");
                                                strcat(buffer, station.departure.c_str());
                                                if (station.depdelay != "0" && station.departure != "None")
                                                {
                                                    strcat(buffer, " (+");
                                                    strcat(buffer, station.depdelay.c_str());
                                                    strcat(buffer, " - ");
                                                    std::string depd = time_plus_delay(station.departure, atoi(station.depdelay.c_str()));
                                                    strcat(buffer, depd.c_str());
                                                    strcat(buffer, ")");
                                                }
                                                strcat(buffer, "\n");
                                            }
                                            else if (is_within_one_hour(time_plus_delay(station.arrival, atoi(station.delay.c_str()))) || is_within_one_hour(time_plus_delay(station.departure, atoi(station.depdelay.c_str()))))
                                            {
                                                count++;
                                                strcat(buffer, "Train: ");
                                                strcat(buffer, train.id.c_str());
                                                strcat(buffer, "\n");
                                                strcat(buffer, "\tFrom: ");
                                                strcat(buffer, train.stations[0].name.c_str());
                                                strcat(buffer, "\n");
                                                strcat(buffer, "\tTo: ");
                                                strcat(buffer, train.stations[train.stations.size() - 1].name.c_str());
                                                strcat(buffer, "\n");
                                                strcat(buffer, "\tStation: ");
                                                strcat(buffer, station.name.c_str());
                                                strcat(buffer, "\n");
                                                strcat(buffer, "\t\tArrival: ");
                                                strcat(buffer, station.arrival.c_str());
                                                if (station.delay != "0")
                                                {
                                                    strcat(buffer, " (+");
                                                    strcat(buffer, station.delay.c_str());
                                                    strcat(buffer, " - ");
                                                    std::string depd = time_plus_delay(station.arrival, atoi(station.delay.c_str()));
                                                    strcat(buffer, depd.c_str());
                                                    strcat(buffer, ")");
                                                }
                                                strcat(buffer, "\n");
                                                strcat(buffer, "\t\tDeparture: ");
                                                strcat(buffer, station.departure.c_str());
                                                if (station.depdelay != "0" && station.departure != "None")
                                                {
                                                    strcat(buffer, " (+");
                                                    strcat(buffer, station.depdelay.c_str());
                                                    strcat(buffer, " - ");
                                                    std::string depd = time_plus_delay(station.departure, atoi(station.depdelay.c_str()));
                                                    strcat(buffer, depd.c_str());
                                                    strcat(buffer, ")");
                                                }
                                                strcat(buffer, "\n");
                                            }
                                        }
                                    }
                                }
                                if (count == 0)
                                {
                                    strcpy(buffer, "No trains passing through ");
                                    strcat(buffer, city.c_str());
                                    strcat(buffer, " in the next hour.\n");
                                }
                                buffer[strlen(buffer)] = '\0';
                            }

                            if (is_valid == 7) // signal <train> <city> <minutes>
                            {
                                char *token = strtok(cmd_copy, " ");
                                token = strtok(NULL, " ");
                                std::string arrival_or_departure = token;
                                token = strtok(NULL, " ");
                                std::string train_id = token;
                                token = strtok(NULL, " ");
                                std::string city = token;
                                token = strtok(NULL, " ");
                                std::string minutes = token;

                                for (auto &train : schedule.trains)
                                {
                                    if (train.id == train_id)
                                    {
                                        for (auto &station : train.stations)
                                        {
                                            if (station.name == city)
                                            {
                                                if (arrival_or_departure == "arrival")
                                                {
                                                    int delay = atoi(station.delay.c_str());
                                                    delay += atoi(minutes.c_str());
                                                    station.delay = std::to_string(delay);
                                                    delay = atoi(station.depdelay.c_str());
                                                    delay += atoi(minutes.c_str());
                                                    station.depdelay = std::to_string(delay);
                                                    bool change = false;
                                                    for (auto &station2 : train.stations)
                                                    {
                                                        if (station2.name == city)
                                                        {
                                                            change = true;
                                                            continue;
                                                        }

                                                        if (change == true)
                                                        {
                                                            int delay2 = atoi(station2.delay.c_str());
                                                            delay2 += atoi(minutes.c_str());
                                                            station2.delay = std::to_string(delay2);
                                                            delay2 = atoi(station2.depdelay.c_str());
                                                            delay2 += atoi(minutes.c_str());
                                                            station2.depdelay = std::to_string(delay2);
                                                        }
                                                    }
                                                }

                                                if (arrival_or_departure == "departure")
                                                {
                                                    int delay = atoi(station.depdelay.c_str());
                                                    delay += atoi(minutes.c_str());
                                                    station.depdelay = std::to_string(delay);
                                                    bool change = false;
                                                    for (auto &station2 : train.stations)
                                                    {
                                                        if (station2.name == city)
                                                        {
                                                            change = true;
                                                            continue;
                                                        }

                                                        if (change == true)
                                                        {
                                                            int delay2 = atoi(station2.depdelay.c_str());
                                                            delay2 += atoi(minutes.c_str());
                                                            station2.depdelay = std::to_string(delay2);
                                                            delay2 = atoi(station2.delay.c_str());
                                                            delay2 += atoi(minutes.c_str());
                                                            station2.delay = std::to_string(delay2);
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                strcpy(buffer, "Train ");
                                strcat(buffer, train_id.c_str());
                                if (arrival_or_departure == "arrival")
                                    strcat(buffer, " will arrive late in ");
                                else
                                    strcat(buffer, " will depart late from ");
                                strcat(buffer, city.c_str());
                                strcat(buffer, ", estimated delay: ");
                                strcat(buffer, minutes.c_str());
                                strcat(buffer, " minutes.\n");

                                pthread_mutex_lock(&updatelock);
                                if (updates.size() == 5)
                                    updates.clear();
                                updates.push_back(buffer);
                                pthread_mutex_unlock(&updatelock);

                                bzero(buffer, 2048);
                                strcpy(buffer, "Delay added successfully.\n");
                                write_backup_xml();
                            }

                            if (is_valid == 8) // trains <city> invalid
                            {
                                strcpy(buffer, "Invalid station.\n");
                            }

                            if (is_valid == 9) // signal <train> <city> <minutes> invalid train id
                            {
                                strcpy(buffer, "Invalid train ID.\n");
                            }

                            if (is_valid == 10) // signal <train> <city> <minutes> train doesnt run today
                            {
                                strcpy(buffer, "This train doesn't run today.\n");
                            }

                            if (is_valid == 11) // signal <train> <city> <minutes> invalid time
                            {
                                strcpy(buffer, "Invalid amount of minutes. Try a positive integer (0,60]\n");
                            }

                            if (is_valid == 12)
                            {
                                char *token = strtok(cmd_copy, " ");
                                token = strtok(NULL, " ");
                                std::string train_id = token;

                                for (auto &train : schedule.trains)
                                {
                                    if (train.id == train_id)
                                    {
                                        for (auto &station : train.stations)
                                        {
                                            station.delay = "0";
                                            station.depdelay = "0";
                                        }
                                    }
                                }

                                strcpy(buffer, "Train ");
                                strcat(buffer, train_id.c_str());
                                strcat(buffer, " has no delays anymore.\n");

                                pthread_mutex_lock(&updatelock);
                                if (updates.size() == 5)
                                    updates.clear();
                                updates.push_back(buffer);
                                pthread_mutex_unlock(&updatelock);

                                bzero(buffer, 2048);
                                strcpy(buffer, "Reset the train's delays successfully\n");
                                write_backup_xml();
                            }

                            if (is_valid == 13)
                            {
                                strcpy(buffer, "Invalid argument. Required arg: arrival or departure.\n");
                            }
                            if ((wbytes = write(client, buffer, strlen(buffer) + 1)) < 0)
                            {
                                printf("[thread %d] error: write() to client.\n");
                                close(client);
                                client_quit = 1;
                                continue;
                            }
                        }
                    }

                    // check if there are any updates
                    pthread_mutex_lock(&updatelock);
                    if (update_count != updates.size())
                    {
                        if (update_count < updates.size())
                        {

                            for (int i = update_count; i < updates.size(); i++)
                            {
                                char buffer[2048];
                                bzero(buffer, 2048);
                                strcpy(buffer, updates[i].c_str());
                                int wbytes;
                                if ((wbytes = write(client, buffer, strlen(buffer) + 1)) < 0)
                                {
                                    printf("[thread %d] error: write() to client.\n");
                                    close(client);
                                    client_quit = 1;
                                    continue;
                                }
                            }
                            update_count = updates.size();
                        }
                        else
                            update_count = 0;
                    }
                    pthread_mutex_unlock(&updatelock);
                }
            }
        }
    }

    printf("[thread %d] shutting down.\n", idx);
    fflush(stdout);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("[server] syntax: %s <threads>\n", argv[0], argv[1]);
        return -1;
    }

    num_threads = atoi(argv[1]);
    if (num_threads <= 0)
    {
        printf("[server] invalid number of threads.\n");
        return -1;
    }

    char file_to_parse[50];
    FILE *f;
    char filepath[] = "timetable_backup.xml";
    if ((f = fopen(filepath, "r")) != NULL)
    {
        printf("We have a backup file! Do you wish to use it for the timetable? y/n\n");
        char choice;
        scanf("%c", &choice);
        if (choice == 'y')
            strcpy(file_to_parse, "timetable_backup.xml");
        else if (choice == 'n')
            strcpy(file_to_parse, "timetable.xml");
        else
        {
            printf("Invalid choice. Exiting...\n");
            return 1;
        }
    }
    else
        strcpy(file_to_parse, "timetable.xml");

    std::ifstream file(file_to_parse);
    if (!file.is_open())
    {
        std::cerr << "[server] missing timetable.xml" << std::endl;
        return 1;
    }

    std::stringstream file_content;
    file_content << file.rdbuf();
    file.close();
    std::string xml = file_content.str();

    std::vector<char> xml_copy(xml.begin(), xml.end());
    xml_copy.push_back('\0');

    rapidxml::xml_document<> doc;
    doc.parse<0>(&xml_copy[0]);

    for (rapidxml::xml_node<> *train_node = doc.first_node("schedule")->first_node("train"); train_node; train_node = train_node->next_sibling())
    {
        Train train;
        train.id = train_node->first_attribute("id")->value();
        train.day = train_node->first_attribute("day")->value();

        for (rapidxml::xml_node<> *station_node = train_node->first_node("station"); station_node; station_node = station_node->next_sibling())
        {
            Station station;
            station.name = station_node->first_node("name")->value();
            bool found = false;
            for (const auto &city : cities)
            {
                if (city == station.name)
                    found = true;
            }
            if (!found)
                cities.push_back(station.name);
            station.arrival = station_node->first_node("arrival")->value();
            station.departure = station_node->first_node("departure")->value();
            if (get_current_day() == train.day)
                station.delay = station_node->first_node("delay")->value();
            else
                station.delay = "0";
            if (get_current_day() == train.day)
                station.depdelay = station_node->first_node("depdelay")->value();
            else
                station.depdelay = "0";
            train.stations.push_back(station);
        }

        schedule.trains.push_back(train);
    }

    file.close();

    sort(cities.begin(), cities.end());

    threadpool = (Thread *)calloc(sizeof(Thread), num_threads);

    struct sockaddr_in server;

    if ((sd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("[server] error: socket().\n");
        return errno;
    }

    int optval = 1;
    setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    bzero(&server, sizeof(server));

    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(PORT);

    if (bind(sd, (struct sockaddr *)&server, sizeof(struct sockaddr)) == -1)
    {
        perror("[server] error: bind().\n");
        return errno;
    }

    if (listen(sd, 5) == -1)
    {
        perror("[server] error: listen().\n");
        return errno;
    }

    printf("[server] creating threadpool.\n");
    for (int i = 0; i < num_threads; i++)
    {
        int *idx = (int *)malloc(sizeof(int));
        *idx = i;
        if (pthread_create(&threadpool[i].t_id, NULL, client_handler, (void *)idx) != 0)
        {
            perror("[server] error: pthread_create().");

            for (int j = 0; j < i; j++)
                pthread_join(threadpool[j].t_id, NULL);
            close(sd);
            free(idx);
            free(threadpool);
            return -1;
        }
    }

    signal(SIGINT, sighandler);

    std::string today = get_current_day();

    while (!sv_shutdown)
    {
        if (today != get_current_day())
        {
            for (auto &train : schedule.trains)
                if (train.day == today)
                    for (auto &station : train.stations)
                    {
                        station.delay = "0";
                        station.depdelay = "0";
                    }
            today = get_current_day();
        }
        sleep(60);
    }

    // cleanup
    printf("[server] closing socket...\n");
    close(sd);

    printf("[server] waiting for threads...\n");
    for (int i = 0; i < num_threads; i++)
    {
        pthread_join(threadpool[i].t_id, NULL);
        printf("[server] thread %d has served %d client(s).\n", i, threadpool[i].t_count);
        printf("[server] thread %d has terminated.\n", i);
    }

    printf("[server] freeing threadpool memory...\n");
    free(threadpool);

    return 0;
}