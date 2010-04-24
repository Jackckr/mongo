// utils.cpp
/*
 *    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */


#include "../stdafx.h"

#include <boost/thread/xtime.hpp>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <assert.h>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>
#include <fcntl.h>

#ifdef _WIN32
# include <io.h>
# define SIGKILL 9
#else
# include <sys/socket.h>
# include <netinet/in.h>
# include <signal.h>
# include <sys/stat.h>
# include <sys/wait.h>
#endif

#include "../client/dbclient.h"
#include "../util/processinfo.h"
#include "utils.h"

extern const char * jsconcatcode_server;

namespace mongo {
#ifdef _WIN32
    inline int close(int fd) { return _close(fd); }
    inline int read(int fd, void* buf, size_t size) { return _read(fd, buf, size); }

    inline int pipe(int fds[2]) { return _pipe(fds, 1024, _O_TEXT | _O_NOINHERIT); }
#endif

    namespace shellUtils {
        
        std::string _dbConnect;
        std::string _dbAuth;
                
        const char *argv0 = 0;
        void RecordMyLocation( const char *_argv0 ) { argv0 = _argv0; }
        
        // helpers
        
        BSONObj makeUndefined() {
            BSONObjBuilder b;
            b.appendUndefined( "" );
            return b.obj();
        }
        const BSONObj undefined_ = makeUndefined();
        
        BSONObj encapsulate( const BSONObj &obj ) {
            return BSON( "" << obj );
        }

        
        // real methods

        
        mongo::BSONObj JSSleep(const mongo::BSONObj &args){
            assert( args.nFields() == 1 );
            assert( args.firstElement().isNumber() );
            int ms = int( args.firstElement().number() );
            {
                auto_ptr< ScriptEngine::Unlocker > u = globalScriptEngine->newThreadUnlocker();
                sleepmillis( ms );
            }
            return undefined_;
        }

        
        BSONObj Quit(const BSONObj& args) {
            // If not arguments are given first element will be EOO, which
            // converts to the integer value 0.
            int exit_code = int( args.firstElement().number() );
            ::exit(exit_code);
            return undefined_;
        }

        BSONObj JSGetMemInfo( const BSONObj& args ){
            ProcessInfo pi;
            uassert( 10258 ,  "processinfo not supported" , pi.supported() );
            
            BSONObjBuilder e;
            e.append( "virtual" , pi.getVirtualMemorySize() );
            e.append( "resident" , pi.getResidentSize() );
            
            BSONObjBuilder b;
            b.append( "ret" , e.obj() );
            
            return b.obj();
        }


#ifndef MONGO_SAFE_SHELL

        BSONObj listFiles(const BSONObj& args){
            uassert( 10257 ,  "need to specify 1 argument to listFiles" , args.nFields() == 1 );
            
            BSONObjBuilder lst;
            
            string rootname = args.firstElement().valuestrsafe();
            path root( rootname );
            stringstream ss;
            ss << "listFiles: no such directory: " << rootname;
            string msg = ss.str();
            uassert( 12581, msg.c_str(), boost::filesystem::exists( root ) );
            
            directory_iterator end;
            directory_iterator i( root);
            
            int num =0;
            while ( i != end ){
                path p = *i;
                BSONObjBuilder b;
                b << "name" << p.string();
                b.appendBool( "isDirectory", is_directory( p ) );
                if ( ! is_directory( p ) ){
                    try { 
                        b.append( "size" , (double)file_size( p ) );
                    }
                    catch ( ... ){
                        i++;
                        continue;
                    }
                }

                stringstream ss;
                ss << num;
                string name = ss.str();
                lst.append( name.c_str(), b.done() );
                num++;
                i++;
            }
            
            BSONObjBuilder ret;
            ret.appendArray( "", lst.done() );
            return ret.obj();
        }


        BSONObj removeFile(const BSONObj& args){
            uassert( 12597 ,  "need to specify 1 argument to listFiles" , args.nFields() == 1 );
            
            bool found = false;
            
            path root( args.firstElement().valuestrsafe() );
            if ( boost::filesystem::exists( root ) ){
                found = true;
                boost::filesystem::remove_all( root );
            }

            BSONObjBuilder b;
            b.appendBool( "removed" , found );
            return b.obj();
        }
        map< int, pair< pid_t, int > > dbs;
        map< pid_t, int > shells;
#ifdef _WIN32
        map< pid_t, HANDLE > handles;
#endif
        
        mongo::mutex mongoProgramOutputMutex;
        stringstream mongoProgramOutput_;

        void writeMongoProgramOutputLine( int port, int pid, const char *line ) {
            mongo::mutex::scoped_lock lk( mongoProgramOutputMutex );
            stringstream buf;
            if ( port > 0 )
                buf << "m" << port << "| " << line;
            else
                buf << "sh" << pid << "| " << line;
            cout << buf.str() << endl;
            mongoProgramOutput_ << buf.str() << endl;
        }
        
        // only returns last 100000 characters
        BSONObj RawMongoProgramOutput( const BSONObj &args ) {
            mongo::mutex::scoped_lock lk( mongoProgramOutputMutex );
            string out = mongoProgramOutput_.str();
            size_t len = out.length();
            if ( len > 100000 )
                out = out.substr( len - 100000, 100000 );
            return BSON( "" << out );
        }

        BSONObj ClearRawMongoProgramOutput( const BSONObj &args ) {
            mongo::mutex::scoped_lock lk( mongoProgramOutputMutex );
            mongoProgramOutput_.str( "" );
            return undefined_;
        }
        
        class ProgramRunner {
            vector<string> argv_;
            int port_;
            int pipe_;
            pid_t pid_;
        public:
            pid_t pid() const { return pid_; }
            ProgramRunner( const BSONObj &args , bool isMongoProgram=true)
            {
                assert( !args.isEmpty() );

                string program( args.firstElement().valuestrsafe() );
                assert( !program.empty() );
                boost::filesystem::path programPath = program;

                if (isMongoProgram){
                    programPath = boost::filesystem::initial_path() / programPath;
#ifdef _WIN32
                    programPath = change_extension(programPath, ".exe");
#endif
                    massert( 10435 ,  "couldn't find " + programPath.native_file_string(), boost::filesystem::exists( programPath ) );
                }

                argv_.push_back( programPath.native_file_string() );
                
                port_ = -1;
                
                BSONObjIterator j( args );
                j.next(); // skip program name (handled above)
                while(j.more()) {
                    BSONElement e = j.next();
                    string str;
                    if ( e.isNumber() ) {
                        stringstream ss;
                        ss << e.number();
                        str = ss.str();
                    } else {
                        assert( e.type() == mongo::String );
                        str = e.valuestr();
                    }
                    if ( str == "--port" )
                        port_ = -2;
                    else if ( port_ == -2 )
                        port_ = strtol( str.c_str(), 0, 10 );
                    argv_.push_back(str);
                }
                
                if ( program != "mongod" && program != "mongos" && program != "mongobridge" )
                    port_ = 0;
                else
                    assert( port_ > 0 );
                if ( port_ > 0 && dbs.count( port_ ) != 0 ){
                    cerr << "count for port: " << port_ << " is not 0 is: " << dbs.count( port_ ) << endl;
                    assert( dbs.count( port_ ) == 0 );        
                }
            }
            
            void start() {
                int pipeEnds[ 2 ];
                assert( pipe( pipeEnds ) != -1 );
                
                fflush( 0 );
                launch_process(pipeEnds[1]); //sets pid_
                
                cout << "shell: started mongo program";
                for (unsigned i=0; i < argv_.size(); i++)
                    cout << " " << argv_[i];
                cout << endl;

                if ( port_ > 0 )
                    dbs.insert( make_pair( port_, make_pair( pid_, pipeEnds[ 1 ] ) ) );
                else
                    shells.insert( make_pair( pid_, pipeEnds[ 1 ] ) );
                pipe_ = pipeEnds[ 0 ];
            }
            
            // Continue reading output
            void operator()() {
                // This assumes there aren't any 0's in the mongo program output.
                // Hope that's ok.
                char buf[ 1024 ];
                char temp[ 1024 ];
                char *start = buf;
                while( 1 ) {
                    int lenToRead = 1023 - ( start - buf );
                    int ret = read( pipe_, (void *)start, lenToRead );
                    assert( ret != -1 );
                    start[ ret ] = '\0';
                    if ( strlen( start ) != unsigned( ret ) )
                        writeMongoProgramOutputLine( port_, pid_, "WARNING: mongod wrote null bytes to output" );
                    char *last = buf;
                    for( char *i = strchr( buf, '\n' ); i; last = i + 1, i = strchr( last, '\n' ) ) {
                        *i = '\0';
                        writeMongoProgramOutputLine( port_, pid_, last );
                    }
                    if ( ret == 0 ) {
                        if ( *last )
                            writeMongoProgramOutputLine( port_, pid_, last );
                        close( pipe_ );
                        break;
                    }
                    if ( last != buf ) {
                        strcpy( temp, last );
                        strcpy( buf, temp );
                    } else {
                        assert( strlen( buf ) <= 1023 );
                    }
                    start = buf + strlen( buf );
                }        
            }
            void launch_process(int child_stdout){
#ifdef _WIN32
                stringstream ss;
                for (int i=0; i < argv_.size(); i++){
                    if (i) ss << ' ';
                    if (argv_[i].find(' ') == string::npos)
                        ss << argv_[i];
                    else
                        ss << '"' << argv_[i] << '"';
                }

                string args = ss.str();
                
                boost::scoped_array<TCHAR> args_tchar (new TCHAR[args.size() + 1]);
                for (size_t i=0; i < args.size()+1; i++)
                    args_tchar[i] = args[i];

                HANDLE h = (HANDLE)_get_osfhandle(child_stdout);
                assert(h != INVALID_HANDLE_VALUE);
                assert(SetHandleInformation(h, HANDLE_FLAG_INHERIT, 1));

                STARTUPINFO si;
                ZeroMemory(&si, sizeof(si));
                si.cb = sizeof(si);
                si.hStdError = h;
                si.hStdOutput = h;
                si.dwFlags |= STARTF_USESTDHANDLES;

                PROCESS_INFORMATION pi;
                ZeroMemory(&pi, sizeof(pi));

                bool success = CreateProcess( NULL, args_tchar.get(), NULL, NULL, true, 0, NULL, NULL, &si, &pi);
                assert(success);

                CloseHandle(pi.hThread);

                pid_ = pi.dwProcessId;
                handles.insert( make_pair( pid_, pi.hProcess ) );

#else

                pid_ = fork();
                assert( pid_ != -1 );
                
                if ( pid_ == 0 ) {
                    // DON'T ASSERT IN THIS BLOCK - very bad things will happen

                    const char** argv = new const char* [argv_.size()+1]; // don't need to free - in child
                    for (unsigned i=0; i < argv_.size(); i++){
                        argv[i] = argv_[i].c_str();
                    }
                    argv[argv_.size()] = 0;
                    
                    if ( dup2( child_stdout, STDOUT_FILENO ) == -1 ||
                         dup2( child_stdout, STDERR_FILENO ) == -1 )
                    {
                        cout << "Unable to dup2 child output: " << OUTPUT_ERRNO() << endl;
                        ::_Exit(-1); //do not pass go, do not call atexit handlers
                    }

                    execvp( argv[ 0 ], const_cast<char**>(argv) );

                    cout << "Unable to start program: " << OUTPUT_ERRNO() << endl;
                    ::_Exit(-1);
                }

#endif
            }
        };
        
        //returns true if process exited
        bool wait_for_pid(pid_t pid, bool block=true, int* exit_code=NULL){
#ifdef _WIN32
            assert(handles.count(pid));
            HANDLE h = handles[pid];

            if (block)
                WaitForSingleObject(h, INFINITE);

            DWORD tmp;
            if(GetExitCodeProcess(h, &tmp)){
                CloseHandle(h);
                handles.erase(pid);
                if (exit_code)
                    *exit_code = tmp;
                return true;
            }else{
                return false;
            }
#else
            int tmp;
            bool ret = (pid == waitpid(pid, &tmp, (block ? 0 : WNOHANG)));
            if (exit_code)
                *exit_code = WEXITSTATUS(tmp);
            return ret;
                
#endif
        }
        BSONObj StartMongoProgram( const BSONObj &a ) {
            _nokillop = true;
            ProgramRunner r( a );
            r.start();
            boost::thread t( r );
            return BSON( string( "" ) << int( r.pid() ) );
        }

        BSONObj RunMongoProgram( const BSONObj &a ) {
            ProgramRunner r( a );
            r.start();
            boost::thread t( r );
            wait_for_pid(r.pid());
            shells.erase( r.pid() );
            return BSON( string( "" ) << int( r.pid() ) );
        }

        BSONObj RunProgram(const BSONObj &a) {
            ProgramRunner r( a, false );
            r.start();
            boost::thread t( r );
            int exit_code;
            wait_for_pid(r.pid(), true,  &exit_code);
            shells.erase( r.pid() );
            return BSON( string( "" ) << exit_code );
        }

        BSONObj ResetDbpath( const BSONObj &a ) {
            assert( a.nFields() == 1 );
            string path = a.firstElement().valuestrsafe();
            assert( !path.empty() );
            if ( boost::filesystem::exists( path ) )
                boost::filesystem::remove_all( path );
            boost::filesystem::create_directory( path );    
            return undefined_;
        }
        
        void copyDir( const path &from, const path &to ) {
            directory_iterator end;
            directory_iterator i( from );
            while( i != end ) {
                path p = *i;
                if ( p.leaf() != "mongod.lock" ) {
                    if ( is_directory( p ) ) {
                        path newDir = to / p.leaf();
                        boost::filesystem::create_directory( newDir );
                        copyDir( p, newDir );
                    } else {
                        boost::filesystem::copy_file( p, to / p.leaf() );
                    }
                }
                ++i;
            }            
        }
        
        // NOTE target dbpath will be cleared first
        BSONObj CopyDbpath( const BSONObj &a ) {
            assert( a.nFields() == 2 );
            BSONObjIterator i( a );
            string from = i.next().str();
            string to = i.next().str();
            assert( !from.empty() );
            assert( !to.empty() );
            if ( boost::filesystem::exists( to ) )
                boost::filesystem::remove_all( to );
            boost::filesystem::create_directory( to );
            copyDir( from, to );
            return undefined_;
        }

        inline void kill_wrapper(pid_t pid, int sig, int port){
#ifdef _WIN32
            if (sig == SIGKILL || port == 0){
                assert( handles.count(pid) );
                TerminateProcess(handles[pid], 1); // returns failure for "zombie" processes.
            }else{
                DBClientConnection conn;
                conn.connect("127.0.0.1:" + BSONObjBuilder::numStr(port));
                try {
                    conn.simpleCommand("admin", NULL, "shutdown");
                } catch (...) {
                    //Do nothing. This command never returns data to the client and the driver doesn't like that.
                }
            }
#else
            assert( 0 == kill( pid, sig ) );
#endif
        }
            
        
        int killDb( int port, pid_t _pid, int signal ) {
            pid_t pid;
            int exitCode = 0;
            if ( port > 0 ) {
                if( dbs.count( port ) != 1 ) {
                    cout << "No db started on port: " << port << endl;
                    return 0;
                }
                pid = dbs[ port ].first;
            } else {
                pid = _pid;
            }
            
            kill_wrapper( pid, signal, port );
            
            int i = 0;
            for( ; i < 65; ++i ) {
                if ( i == 5 ) {
                    char now[64];
                    time_t_to_String(time(0), now);
                    now[ 20 ] = 0;
                    cout << now << " process on port " << port << ", with pid " << pid << " not terminated, sending sigkill" << endl;
                    kill_wrapper( pid, SIGKILL, port );
                }        
                if(wait_for_pid(pid, false, &exitCode))
                    break;
                sleepmillis( 1000 );
            }
            if ( i == 65 ) {
                char now[64];
                time_t_to_String(time(0), now);
                now[ 20 ] = 0;
                cout << now << " failed to terminate process on port " << port << ", with pid " << pid << endl;
                assert( "Failed to terminate process" == 0 );
            }

            if ( port > 0 ) {
                close( dbs[ port ].second );
                dbs.erase( port );
            } else {
                close( shells[ pid ] );
                shells.erase( pid );
            }
            if ( i > 4 || signal == SIGKILL ) {
                sleepmillis( 4000 ); // allow operating system to reclaim resources
            }
            
            return exitCode;
        }

        int getSignal( const BSONObj &a ) {
            int ret = SIGTERM;
            if ( a.nFields() == 2 ) {
                BSONObjIterator i( a );
                i.next();
                BSONElement e = i.next();
                assert( e.isNumber() );
                ret = int( e.number() );
            }
            return ret;
        }
        
        BSONObj StopMongoProgram( const BSONObj &a ) {
            assert( a.nFields() == 1 || a.nFields() == 2 );
            assert( a.firstElement().isNumber() );
            int port = int( a.firstElement().number() );
            int code = killDb( port, 0, getSignal( a ) );
            cout << "shell: stopped mongo program on port " << port << endl;
            return BSON( "" << code );
        }        
        
        BSONObj StopMongoProgramByPid( const BSONObj &a ) {
            assert( a.nFields() == 1 || a.nFields() == 2 );
            assert( a.firstElement().isNumber() );
            int pid = int( a.firstElement().number() );            
            int code = killDb( 0, pid, getSignal( a ) );
            cout << "shell: stopped mongo program on pid " << pid << endl;
            return BSON( "" << code );
        }
                                                
        void KillMongoProgramInstances() {
            vector< int > ports;
            for( map< int, pair< pid_t, int > >::iterator i = dbs.begin(); i != dbs.end(); ++i )
                ports.push_back( i->first );
            for( vector< int >::iterator i = ports.begin(); i != ports.end(); ++i )
                killDb( *i, 0, SIGTERM );            
            vector< pid_t > pids;
            for( map< pid_t, int >::iterator i = shells.begin(); i != shells.end(); ++i )
                pids.push_back( i->first );
            for( vector< pid_t >::iterator i = pids.begin(); i != pids.end(); ++i )
                killDb( 0, *i, SIGTERM );
        }
#else // ndef MONGO_SAFE_SHELL
        void KillMongoProgramInstances() {}
#endif
        
        MongoProgramScope::~MongoProgramScope() {
            DESTRUCTOR_GUARD(
                KillMongoProgramInstances();
                ClearRawMongoProgramOutput( BSONObj() );
            )
        }

        unsigned _randomSeed;
        
        BSONObj JSSrand( const BSONObj &a ) {
            uassert( 12518, "srand requires a single numeric argument",
                    a.nFields() == 1 && a.firstElement().isNumber() );
            _randomSeed = (unsigned)a.firstElement().numberLong(); // grab least significant digits
            return undefined_;
        }
        
        BSONObj JSRand( const BSONObj &a ) {
            uassert( 12519, "rand accepts no arguments", a.nFields() == 0 );
            unsigned r;
#if !defined(_WIN32)
            r = rand_r( &_randomSeed );
#else
            r = rand(); // seed not used in this case
#endif
            return BSON( "" << double( r ) / ( double( RAND_MAX ) + 1 ) );
        }

        BSONObj isWindows(const BSONObj& a){
            uassert( 13006, "isWindows accepts no arguments", a.nFields() == 0 );
#ifdef _WIN32
            return BSON( "" << true );
#else
            return BSON( "" << false );
#endif
        }
        
        void installShellUtils( Scope& scope ){
            scope.injectNative( "sleep" , JSSleep );
            scope.injectNative( "quit", Quit );
            scope.injectNative( "getMemInfo" , JSGetMemInfo );
            scope.injectNative( "_srand" , JSSrand );
            scope.injectNative( "_rand" , JSRand );
            scope.injectNative( "_isWindows" , isWindows );

#ifndef MONGO_SAFE_SHELL
            //can't launch programs
            scope.injectNative( "_startMongoProgram", StartMongoProgram );
            scope.injectNative( "runProgram", RunProgram );
            scope.injectNative( "runMongoProgram", RunMongoProgram );
            scope.injectNative( "stopMongod", StopMongoProgram );
            scope.injectNative( "stopMongoProgram", StopMongoProgram );        
            scope.injectNative( "stopMongoProgramByPid", StopMongoProgramByPid );        
            scope.injectNative( "rawMongoProgramOutput", RawMongoProgramOutput );
            scope.injectNative( "clearRawMongoProgramOutput", ClearRawMongoProgramOutput );

            //can't access filesystem
            scope.injectNative( "removeFile" , removeFile );
            scope.injectNative( "listFiles" , listFiles );
            scope.injectNative( "resetDbpath", ResetDbpath );
            scope.injectNative( "copyDbpath", CopyDbpath );
#endif
        }

        void initScope( Scope &scope ) {
            scope.externalSetup();
            mongo::shellUtils::installShellUtils( scope );
            scope.execSetup( jsconcatcode_server , "setupServerCode" );
            
            if ( !_dbConnect.empty() ) {
                uassert( 12513, "connect failed", scope.exec( _dbConnect , "(connect)" , false , true , false ) );
                if ( !_dbAuth.empty() ) {
                    installGlobalUtils( scope );
                    uassert( 12514, "login failed", scope.exec( _dbAuth , "(auth)" , true , true , false ) );
                }
            }
        }
        
        map< const void*, string > _allMyUris;        
        bool _nokillop = false;
        void onConnect( DBClientWithCommands &c ) {
            if ( _nokillop ) {
                return;
            }
            BSONObj info;
            if ( c.runCommand( "admin", BSON( "whatsmyuri" << 1 ), info ) ) {
                // There's no way to explicitly disconnect a DBClientConnection, but we might allocate
                // a new uri on automatic reconnect.  So just store one uri per connection.
                _allMyUris[ &c ] = info[ "you" ].str();
            }
        }
    }
}
