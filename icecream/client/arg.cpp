/* -*- c-file-style: "java"; indent-tabs-mode: nil; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */


#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <cassert>

#include <sys/stat.h>

#include "logging.h"
#include "util.h"
#include "filename.h"
#include "arg.h"

using namespace std;

#define CLIENT_DEBUG 0

#if CLIENT_DEBUG
static string concat_args( const list<string> &list )
{
    int len = list.size() - 1;
    string result = "\"";
    for ( std::list<std::string>::const_iterator it = list.begin();
          it != list.end(); ++it, len-- ) {
        result += *it;
        if ( len )
            result += ", ";
    }
    result += "\"";
    return result;
}
#endif

bool analyse_argv( const char * const *argv,
                   CompileJob &job )
{
    ArgumentsList args;
    string ofile;

#if CLIENT_DEBUG
    trace() << "scanning arguments ";
    for ( int index = 0; argv[index]; index++ )
        trace() << argv[index] << " ";
    trace() << endl;
#endif

    bool always_local = false;
    bool seen_c = false;
    bool seen_s = false;

    for (int i = 0; argv[i]; i++) {
        const char *a = argv[i];

        if (a[0] == '-') {
            if (!strcmp(a, "-E")) {
                always_local = true;
                args.append(a, Arg_Local);
            } else if (!strcmp(a, "-MD") || !strcmp(a, "-MMD")) {
                args.append(a, Arg_Local);
                /* These two generate dependencies as a side effect.  They
                 * should work with the way we call cpp. */
            } else if (!strcmp(a, "-MG") || !strcmp(a, "-MP")) {
                args.append(a, Arg_Local);
                /* These just modify the behaviour of other -M* options and do
                 * nothing by themselves. */
            } else if (!strcmp(a, "-MF") || !strcmp(a, "-MT") ||
                       !strcmp(a, "-MQ")) {
                args.append(a, Arg_Local);
                args.append( argv[++i], Arg_Local );
                /* as above but with extra argument */
            } else if (a[1] == 'M') {
                /* -M(anything else) causes the preprocessor to
                    produce a list of make-style dependencies on
                    header files, either to stdout or to a local file.
                    It implies -E, so only the preprocessor is run,
                    not the compiler.  There would be no point trying
                    to distribute it even if we could. */
                always_local = true;
                args.append(a, Arg_Local);
            } else if (str_startswith("-Wa,", a)) {
                /* Options passed through to the assembler.  The only one we
                 * need to handle so far is -al=output, which directs the
                 * listing to the named file and cannot be remote.  Parsing
                 * all the options would be complex since you can give several
                 * comma-separated assembler options after -Wa, but looking
                 * for '=' should be safe. */
                if (strchr(a, '=')) {
                    always_local = true;
                    args.append(a, Arg_Local);
                } else
                    args.append(a, Arg_Remote);
            } else if (!strcmp(a, "-S")) {
                seen_s = true;
            } else if (!strcmp(a, "-fprofile-arcs")
                       || !strcmp(a, "-ftest-coverage")) {
#if CLIENT_DEBUG
                log_info() << "compiler will emit profile info; must be local" << endl;
#endif
                always_local = true;
                args.append(a, Arg_Local);
            } else if (!strcmp(a, "-x")) {
#if CLIENT_DEBUG
                log_info() << "gcc's -x handling is complex; running locally" << endl;
#endif
                always_local = true;
                args.append(a, Arg_Local);
            } else if (!strcmp(a, "-c")) {
                seen_c = true;
            } else if (str_startswith("-o", a)) {
                assert( ofile.empty() );
                if (!strcmp(a, "-o")) {
                    /* Whatever follows must be the output */
                    ofile = argv[++i];
                } else {
                    a += 2;
                    ofile = a;
                }
            } else if (str_equal("-D", a)
                       || str_equal("-I", a)
                       || str_equal("-U", a)
                       || str_equal("-L", a)
                       || str_equal("-l", a)
                       || str_equal("-MF", a)
                       || str_equal("-MT", a)
                       || str_equal("-MQ", a)
                       || str_equal("-include", a)
                       || str_equal("-imacros", a)
                       || str_equal("-iprefix", a)
                       || str_equal("-iwithprefix", a)
                       || str_equal("-isystem", a)
                       || str_equal("-iwithprefixbefore", a)
                       || str_equal("-idirafter", a) ) {
                args.append(a, Arg_Local);
                /* skip next word, being option argument */
                if (argv[i+1])
                    args.append(  argv[++i], Arg_Local );
            } else if (str_startswith("-Wp,", a)
                 || str_startswith("-D", a)
                 || str_startswith("-U", a)
                 || str_startswith("-I", a)
                 || str_startswith("-l", a)
                 || str_startswith("-L", a)) {
                args.append(a, Arg_Local);
            } else if (str_equal("-undef", a)
                 || str_equal("-nostdinc", a)
                 || str_equal("-nostdinc++", a)
                 || str_equal("-MD", a)
                 || str_equal("-MMD", a)
                 || str_equal("-MG", a)
                 || str_equal("-MP", a)) {
                args.append(a, Arg_Local);
            } else
                args.append( a, Arg_Rest );
        } else {
            args.append( a, Arg_Rest );
        }
    }

    if (!seen_c && !seen_s)
        always_local = true;
    else if ( seen_s ) {
        if ( seen_c )
            log_error() << "can't have both -c and -S, ignoring -c" << endl;
        args.append( "-S", Arg_Remote );
    } else
        args.append( "-c", Arg_Remote );

    if (ofile == "-" ) {
        /* Different compilers may treat "-o -" as either "write to
         * stdout", or "write to a file called '-'".  We can't know,
         * so we just always run it locally.  Hopefully this is a
         * pretty rare case. */
#ifdef CLIENT_DEBUG
        log_info() << "output to stdout?  running locally" << endl;
#endif
        always_local = true;
    }

    string compiler_name = find_basename( args.front().first );
    args.pop_front(); // away!

    job.setLanguage( CompileJob::Lang_C );
    if ( ( compiler_name.size() > 2 &&
         compiler_name.substr( compiler_name.size() - 2 ) == "++" ) || compiler_name == "CC" )
        job.setLanguage( CompileJob::Lang_CXX );

    if ( seen_c || seen_s ) {
        /* TODO: ccache has the heuristic of ignoring arguments that are not
         * extant files when looking for the input file; that's possibly
         * worthwile.  Of course we can't do that on the server. */
        string ifile;
        for ( ArgumentsList::iterator it = args.begin();
              it != args.end(); ) {
            if ( it->first.at( 0 ) == '-' )
                ++it;
            else if ( ifile.empty() ) {
                job.setInputFile( it->first );
                ifile = it->first;
                it = args.erase( it );
            } else {
                log_info() << "found another non option on command line. Two input files? " << it->first << endl;
                abort();
                ++it;
            }
        }

        if ( ifile.find( '.' ) != string::npos ) {
            string::size_type dot_index = ifile.find_last_of( '.' );
            string ext = ifile.substr( dot_index + 1 );

            if (ext == "i" || ext == "c") {
#if CLIENT_DEBUG
                if ( job.language() != CompileJob::Lang_C )
                    log_info() << "switching to C for " << ifile << endl;
#endif
                job.setLanguage( CompileJob::Lang_C );
            } else if (ext == "c" || ext == "cc"
                       || ext == "cpp" || ext == "cxx"
                       || ext == "cp" || ext == "c++"
                       || ext == "C" || ext == "ii") {
#if CLIENT_DEBUG
                if ( job.language() != CompileJob::Lang_CXX )
                    log_info() << "switching to C++ for " << ifile << endl;
#endif
                job.setLanguage( CompileJob::Lang_CXX );
            } else if(ext == "mi" || ext == "m"
                      || ext == "mii" || ext == "mm"
                      || ext == "M" )
                job.setLanguage( CompileJob::Lang_OBJC );

            if ( ofile.empty() ) {
                ofile = ifile.substr( 0, dot_index ) + ".o";
                string::size_type slash = ofile.find_last_of( '/' );
                if ( slash != string::npos )
                    ofile = ofile.substr( slash + 1 );
            }
        }
    } else
        assert( always_local );

    job.setFlags( args );
    job.setOutputFile( ofile );

#if CLIENT_DEBUG
    trace() << "scanned result: local args=" << concat_args( job.localFlags() )
            << ", remote args=" << concat_args( job.remoteFlags() )
            << ", rest=" << concat_args( job.restFlags() )
            << ", local=" << always_local
            << ", lang=" << ( job.language() == CompileJob::Lang_CXX ? "C++" : "C" )
            << endl;
#endif

    return always_local;
}
