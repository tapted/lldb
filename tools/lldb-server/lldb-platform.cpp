//===-- lldb-platform.cpp ---------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/lldb-python.h"

// C Includes
#include <errno.h>
#if defined(__APPLE__)
#include <netinet/in.h>
#endif
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

// C++ Includes

// Other libraries and framework includes
#include "lldb/Core/Error.h"
#include "lldb/Core/ConnectionMachPort.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Host/ConnectionFileDescriptor.h"
#include "lldb/Host/HostGetOpt.h"
#include "lldb/Host/OptionParser.h"
#include "lldb/Host/Socket.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "Plugins/Process/gdb-remote/GDBRemoteCommunicationServerPlatform.h"
#include "Plugins/Process/gdb-remote/ProcessGDBRemoteLog.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::process_gdb_remote;

//----------------------------------------------------------------------
// option descriptors for getopt_long_only()
//----------------------------------------------------------------------

static int g_debug = 0;
static int g_verbose = 0;
static int g_server = 0;

static struct option g_long_options[] =
{
    { "debug",              no_argument,        &g_debug,           1   },
    { "verbose",            no_argument,        &g_verbose,         1   },
    { "listen",             required_argument,  NULL,               'L' },
    { "port-offset",        required_argument,  NULL,               'p' },
    { "gdbserver-port",     required_argument,  NULL,               'P' },
    { "min-gdbserver-port", required_argument,  NULL,               'm' },
    { "max-gdbserver-port", required_argument,  NULL,               'M' },
    { "lldb-command",       required_argument,  NULL,               'c' },
    { "server",             no_argument,        &g_server,          1   },
    { NULL,                 0,                  NULL,               0   }
};

#if defined (__APPLE__)
#define LOW_PORT    (IPPORT_RESERVED)
#define HIGH_PORT   (IPPORT_HIFIRSTAUTO)
#else
#define LOW_PORT    (1024u)
#define HIGH_PORT   (49151u)
#endif


//----------------------------------------------------------------------
// Watch for signals
//----------------------------------------------------------------------
static void
signal_handler(int signo)
{
    switch (signo)
    {
    case SIGHUP:
        // Use SIGINT first, if that does not work, use SIGHUP as a last resort.
        // And we should not call exit() here because it results in the global destructors
        // to be invoked and wreaking havoc on the threads still running.
        Host::SystemLog(Host::eSystemLogWarning, "SIGHUP received, exiting lldb-server...\n");
        abort();
        break;
    }
}

static void
display_usage (const char *progname, const char *subcommand)
{
    fprintf(stderr, "Usage:\n  %s %s [--log-file log-file-path] [--log-flags flags] --listen port\n", progname, subcommand);
    exit(0);
}

//----------------------------------------------------------------------
// main
//----------------------------------------------------------------------
int
main_platform (int argc, char *argv[])
{
    const char *progname = argv[0];
    const char *subcommand = argv[1];
    argc--;
    argv++;
    signal (SIGPIPE, SIG_IGN);
    signal (SIGHUP, signal_handler);
    int long_option_index = 0;
    Error error;
    std::string listen_host_port;
    int ch;

    lldb::DebuggerSP debugger_sp = Debugger::CreateInstance ();

    debugger_sp->SetInputFileHandle(stdin, false);
    debugger_sp->SetOutputFileHandle(stdout, false);
    debugger_sp->SetErrorFileHandle(stderr, false);
    
    GDBRemoteCommunicationServerPlatform::PortMap gdbserver_portmap;
    int min_gdbserver_port = 0;
    int max_gdbserver_port = 0;
    uint16_t port_offset = 0;
    
    std::vector<std::string> lldb_commands;
    bool show_usage = false;
    int option_error = 0;
    int socket_error = -1;
    
    std::string short_options(OptionParser::GetShortOptionString(g_long_options));
                            
#if __GLIBC__
    optind = 0;
#else
    optreset = 1;
    optind = 1;
#endif

    while ((ch = getopt_long_only(argc, argv, short_options.c_str(), g_long_options, &long_option_index)) != -1)
    {
        switch (ch)
        {
        case 0:   // Any optional that auto set themselves will return 0
            break;

        case 'L':
            listen_host_port.append (optarg);
            break;

        case 'p':
            {
                char *end = NULL;
                long tmp_port_offset = strtoul(optarg, &end, 0);
                if (end && *end == '\0')
                {
                    if (LOW_PORT <= tmp_port_offset && tmp_port_offset <= HIGH_PORT)
                    {
                        port_offset = (uint16_t)tmp_port_offset;
                    }
                    else
                    {
                        fprintf (stderr, "error: port offset %li is not in the valid user port range of %u - %u\n", tmp_port_offset, LOW_PORT, HIGH_PORT);
                        option_error = 5;
                    }
                }
                else
                {
                    fprintf (stderr, "error: invalid port offset string %s\n", optarg);
                    option_error = 4;
                }
            }
            break;
                
        case 'P':
        case 'm':
        case 'M':
            {
                char *end = NULL;
                long portnum = strtoul(optarg, &end, 0);
                if (end && *end == '\0')
                {
                    if (LOW_PORT <= portnum && portnum <= HIGH_PORT)
                    {
                        if (ch  == 'P')
                            gdbserver_portmap[(uint16_t)portnum] = LLDB_INVALID_PROCESS_ID;
                        else if (ch == 'm')
                            min_gdbserver_port = portnum;
                        else
                            max_gdbserver_port = portnum;
                    }
                    else
                    {
                        fprintf (stderr, "error: port number %li is not in the valid user port range of %u - %u\n", portnum, LOW_PORT, HIGH_PORT);
                        option_error = 1;
                    }
                }
                else
                {
                    fprintf (stderr, "error: invalid port number string %s\n", optarg);
                    option_error = 2;
                }
            }
            break;
            
        case 'c':
            lldb_commands.push_back(optarg);
            break;

        case 'h':   /* fall-through is intentional */
        case '?':
            show_usage = true;
            break;
        }
    }

    // Make a port map for a port range that was specified.
    if (min_gdbserver_port < max_gdbserver_port)
    {
        for (uint16_t port = min_gdbserver_port; port < max_gdbserver_port; ++port)
            gdbserver_portmap[port] = LLDB_INVALID_PROCESS_ID;
    }
    else if (min_gdbserver_port != max_gdbserver_port)
    {
        fprintf (stderr, "error: --min-gdbserver-port (%u) is greater than --max-gdbserver-port (%u)\n", min_gdbserver_port, max_gdbserver_port);
        option_error = 3;
        
    }

    // Print usage and exit if no listening port is specified.
    if (listen_host_port.empty())
        show_usage = true;
    
    if (show_usage || option_error)
    {
        display_usage(progname, subcommand);
        exit(option_error);
    }
    
    // Execute any LLDB commands that we were asked to evaluate.
    for (const auto &lldb_command : lldb_commands)
    {
        lldb_private::CommandReturnObject result;
        printf("(lldb) %s\n", lldb_command.c_str());
        debugger_sp->GetCommandInterpreter().HandleCommand(lldb_command.c_str(), eLazyBoolNo, result);
        const char *output = result.GetOutputData();
        if (output && output[0])
            puts(output);
    }

    std::unique_ptr<Socket> listening_socket_up;
    Socket *socket = nullptr;
    printf ("Listening for a connection from %s...\n", listen_host_port.c_str());
    const bool children_inherit_listen_socket = false;

    // the test suite makes many connections in parallel, let's not miss any.
    // The highest this should get reasonably is a function of the number 
    // of target CPUs.  For now, let's just use 100
    const int backlog = 100;
    error = Socket::TcpListen(listen_host_port.c_str(), children_inherit_listen_socket, socket, NULL, backlog);
    if (error.Fail())
    {
        printf("error: %s\n", error.AsCString());
        exit(socket_error);
    }
    listening_socket_up.reset(socket);

    do {
        GDBRemoteCommunicationServerPlatform platform;
        
        if (port_offset > 0)
            platform.SetPortOffset(port_offset);

        if (!gdbserver_portmap.empty())
        {
            platform.SetPortMap(std::move(gdbserver_portmap));
        }

        const bool children_inherit_accept_socket = true;
        socket = nullptr;
        error = listening_socket_up->BlockingAccept(listen_host_port.c_str(), children_inherit_accept_socket, socket);
        if (error.Fail())
        {
            printf ("error: %s\n", error.AsCString());
            exit(socket_error);
        }
        printf ("Connection established.\n");
        if (g_server)
        {
            // Collect child zombie processes.
            while (waitpid(-1, nullptr, WNOHANG) > 0);
            if (fork())
            {
                // Parent doesn't need a connection to the lldb client
                delete socket;
                socket = nullptr;

                // Parent will continue to listen for new connections.
                continue;
            }
            else
            {
                // Child process will handle the connection and exit.
                g_server = 0;
                // Listening socket is owned by parent process.
                listening_socket_up.release();
            }
        }
        else
        {
            // If not running as a server, this process will not accept
            // connections while a connection is active.
            listening_socket_up.reset();
        }
        platform.SetConnection (new ConnectionFileDescriptor(socket));

        if (platform.IsConnected())
        {
            // After we connected, we need to get an initial ack from...
            if (platform.HandshakeWithClient(&error))
            {
                bool interrupt = false;
                bool done = false;
                while (!interrupt && !done)
                {
                    if (platform.GetPacketAndSendResponse (UINT32_MAX, error, interrupt, done) != GDBRemoteCommunication::PacketResult::Success)
                        break;
                }

                if (error.Fail())
                {
                    fprintf(stderr, "error: %s\n", error.AsCString());
                }
            }
            else
            {
                fprintf(stderr, "error: handshake with client failed\n");
            }
        }
    } while (g_server);

    fprintf(stderr, "lldb-server exiting...\n");

    return 0;
}
