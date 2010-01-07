// (c) 2009 Alexander Holler
// See the file COPYING for copying permission.
//
// I'm believing in KISS (keep it stupid, simple).
//
// Just as a note, we assume the XML is sorted in some ways,
// i.e. the id and title of a page is defined before any revision.
// This keeps the parser simple.
//
#include <malloc.h> // mallinfo()
#include <iostream>
#include <fstream>
#include <string>
#include <stack>
#include <map>
#include <tr1/unordered_map> // You will need a gcc >= 3.x

#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/filter/bzip2.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options/cmdline.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>

#include <expat.h>

#include "version.h"

#define BUFFER_SIZE 1024*1024

// Stuff we are reading and feeding to git.
static std::string comment;
static std::string ip;
static std::string text;
static std::string timestamp;
static std::string title;
static std::string title_ns;
static std::string username;
static bool is_minor(false);
static bool is_del(false); // TODO: is currently never set.
static std::string id_contributor;
static std::string id_page;
static std::string id_revision;
boost::posix_time::ptime time_start;

// Options.
static std::string filename;
static std::string tempfilename;
static std::string committer("wp2git <wp2git@localhost.localdomain>");
static unsigned deepness(3);
static bool wikitime(false);
static size_t max_revisions(0);
static std::string programname;
static std::string blacklist;
static unsigned long revisions_total(0);

// The actual code starts here.

static std::fstream tfile;
static size_t revisions_read(0);

enum Element {
    Element_unknown,
    Element_comment,
    Element_id,
    Element_ip,
    Element_minor,
    Element_revision,
    Element_text,
    Element_timestamp,
    Element_title,
    Element_username,
    // The stuff below doesn't interest us.
/*
    Element_contributor,
    Element_mediawiki,
    Element_namespace,
    Element_page,
    Element_restrictions,
*/
};

typedef std::tr1::unordered_map<std::string, Element> MapElementsKeyString;
static MapElementsKeyString mapElementNames;

static void initMap(void)
{
    assert( mapElementNames.empty() );
    mapElementNames["comment"] = Element_comment;
    mapElementNames["id"] = Element_id;
    mapElementNames["ip"] = Element_ip;
    mapElementNames["minor"] = Element_minor;
    mapElementNames["revision"] = Element_revision;
    mapElementNames["text"] = Element_text;
    mapElementNames["timestamp"] = Element_timestamp;
    mapElementNames["title"] = Element_title;
    mapElementNames["username"] = Element_username;
    // The stuff below doesn't interest us.
/*
    mapElementNames["contributor"] = Element_contributor;
    mapElementNames["mediawiki"]=Element_mediawiki;
    mapElementNames["namespace"] = Element_namespace;
    mapElementNames["page"] = Element_page;
    mapElementNames["restrictions"] = Element_restrictions;
*/
}

static std::stack<Element> elementStack;

typedef std::multimap<std::time_t, std::string> Revisions;
static Revisions revisions;

typedef std::multimap<std::time_t, std::streampos> RevisionPositions;
static RevisionPositions revisionPositions;

static std::string actualValue;

static std::set<std::string> ns_blacklist;
static bool ignorePage(false); // Will be set to true if the title of page is found in the blacklist
static unsigned long ignoredPages(0);
static unsigned long ignoredRevisions(0);

static void printHelp(const std::string& myName,
    const boost::program_options::options_description& desc)
{
    std::cerr << "Usage:" << std::endl;
    std::cerr << std::endl;
    std::cerr << myName << " [options] | GIT_DIR=repo git fast-import" << std::endl;
    std::cerr << std::endl;
    std::cerr << desc << std::endl;
    std::cerr << std::endl;
    std::cerr << "Examples:" << std::endl;
    std::cerr << std::endl;
    std::cerr << myName
        << " barwiki-20091206-pages-articles.xml.bz2 | GIT_DIR=repo git fast-import" << std::endl;
    std::cerr << "7z e -bd -so dewiki-20091223-pages-meta-history.xml.7z | "
        << myName << " -t mytempfile -r 62133749 | GIT_DIR=repo git fast-import" << std::endl;
    std::cerr << "7z e -bd -so dewiki-20091028-pages-meta-history.xml.7z | "
        << myName << " -m 100000 -b blacklist.example | bzip2 >stream_for_git-fast-import.bz2" << std::endl;
    std::cerr << myName
        << " -c \"Foo Bar <foo@bar.local>\" -d 10 -w barwiki-20091206-pages-articles.xml.bz2 | GIT_DIR=repo git fast-import" << std::endl;
    std::cerr << myName
        << " show-me-only-page-titles.xml.bz2 >/dev/null" << std::endl;
    std::cerr << std::endl;
    return;
}

int config(int argc, char** argv)
{
    {
    size_t pos = std::string(argv[0]).rfind('/');
    if(pos == std::string::npos)
        programname=argv[0];
    else
        programname=std::string(argv[0]).substr(pos+1);
    }
    boost::program_options::variables_map vm;
    boost::program_options::options_description desc("Allowed options");
    try {
        desc.add_options()
        ("help,h", "help message")
        ("blacklist,b", boost::program_options::value<std::string>(&blacklist),
            "Filename of a blacklist for namespaces (default none)")
        ("committer,c", boost::program_options::value<std::string>(&committer),
            std::string("git \"Committer\" used while doing the commits (default \"" + committer + "\")").c_str())
        ("deepness,d", boost::program_options::value<unsigned>(&deepness),
            "The deepness of the result directory structure (default 3)")
        ("max,m", boost::program_options::value<size_t>(&max_revisions),
            "Maximum number of revisions (not pages!) to import (default 0 = all)")
        ("revisions,r", boost::program_options::value<unsigned long>(&revisions_total),
            "The total number of revisions (used to calc ETA)")
        ("tempfile,t", boost::program_options::value<std::string>(&tempfilename),
            "Use this temporary file to minimize RAM-usage")
        ("wikitime,w", boost::program_options::bool_switch(&wikitime),
            "TODO: If true, the commit time will be set to the revision creation, not the current system time (default false)")
        ("mediawiki-export-bz2", boost::program_options::value< std::vector<std::string> >(), "file to read")
        ;
        boost::program_options::positional_options_description podesc;
        podesc.add("mediawiki-export-bz2", -1);
        boost::program_options::store(
            boost::program_options::command_line_parser(argc, argv).options(
                desc).positional(podesc).run(), vm);
        boost::program_options::notify(vm);
    }
    catch (std::exception& e) {
        printHelp(programname, desc);
        // std::cerr << "ERROR: " << e.what() << std::endl;
        return 2;
    }
    if(vm.count("help")) {
        printHelp(programname, desc);
        return 1;
    }
    if( vm.count("mediawiki-export-bz2") > 1 ) {
        printHelp(programname, desc);
        return 3;
    }
    else if(vm.count("mediawiki-export-bz2") == 1 )
        filename = vm["mediawiki-export-bz2"].as< std::vector<std::string> >()[0];
    if( ! max_revisions )
        max_revisions = (unsigned long)-1;
    else
        revisions_total = max_revisions;
    return 0;
}

inline std::string asciiize_char(char c)
{
    static const std::string allowedChars(
        //"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789()-_"
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"
    );
    if( allowedChars.find(c) != std::string::npos )
        return std::string(1, c);
    std::string s(".");
    static const std::string hexChars("0123456789ABCDEF");
    s += hexChars[(c>>4) & 0x0f];
    s += hexChars[c & 0x0f];
    return s;
}

static std::string asciiize(const std::string& str)
{
    std::string s;
    size_t end = str.size();
    for(size_t i=0; i<end; ++i)
        s += asciiize_char(str[i]);
    return s;
}

static std::string buildCommitString(std::time_t date)
{
    std::string str("author ");
    if( ! username.empty() ) {
        str += username;
        str += " <uid-" + id_contributor;
    }
    else {
        str += ip;
        str += " <ip";
    }
    str += "@git.bar.wikipedia.org> ";
    str += boost::lexical_cast<std::string>(date);
    // TODO: Fix timezone (using boost::local_time).
    str += " +0000\n";
    str += "committer " + committer + ' ';
    // TODO: Fix timezone (using boost::local_time).
    str += boost::lexical_cast<std::string>(time(NULL))+ " +0100\n";
    std::string commit_comment("\n\nwp2git import of"
        " page " + id_page
        + " rev " + id_revision
        + (is_minor ? " (minor)" : "")
        + ".\n");
    std::string commit_title(title_ns);
    if( ! commit_title.empty() )
        commit_title += ':';
    commit_title += title + "\n\n";
    str += "data " + boost::lexical_cast<std::string>(commit_title.size() + comment.size() + commit_comment.size()) + '\n';
    str += commit_title + comment + commit_comment + '\n';

    // Build the filename
    unsigned flags(0);
    if(is_minor)
        ++flags;
    if( username.empty() )
        flags += 2;
    if(is_del)
        flags += 4;
    std::string tfilename(boost::lexical_cast<std::string>(flags));
    // TODO: import.py does something other here (e.g. 10-)
    tfilename += "-" + title_ns + '/';
    for( unsigned i = 0; i<deepness && i<title.size(); ++i ) {
        tfilename += asciiize_char(title[i]);
        tfilename += '/';
    }
    tfilename += asciiize(title);
    tfilename += ".mediawiki";

    str += "M 100644 :" + id_revision
        + ' ' + tfilename;
    return str;
}

static std::streampos writeString(const std::string& str)
{
    std::streampos pos;
    try {
        pos = tfile.tellp();
        size_t len = str.size();
        tfile.write(reinterpret_cast<const char*>(&len), sizeof(len));
        tfile.write(str.data(), str.size());
    }
    catch (std::exception& e) {
        // e.what() offers only cryptic errors here
        std::cerr << "ERROR: Can't write to file '" << tempfilename << "'!" << std::endl;
        exit(3);
    }
    return pos;
}

static std::string readString(std::streampos pos)
{
   try {
        tfile.seekp(pos);
        size_t len;
        tfile.read(reinterpret_cast<char*>(&len), sizeof(len));
        if(!len)
            return "";
        std::vector<char> v(len);
        tfile.read(&v[0], len);
        return std::string(v.begin(), v.end());
    }
    catch (std::exception& e) {
        // e.what() offers only cryptic errors here
        std::cerr << "ERROR: Can't read from file '" << tempfilename << "'!" << std::endl;
        exit(4);
    }
}

static std::time_t time_t_from_timestamp(void)
{
    // We assume the following format for timestamps: 2009-12-01T12:09:31Z
    assert( timestamp.size() == 20 );
    timestamp[10]=' '; // Exchange T.
    timestamp.erase(19); // Remove Z.
    // time_t starts counting 1.1.1970.
    static const boost::posix_time::ptime time_t_epoch(boost::gregorian::date(1970,1,1));
    // Build the time_t, is just the difference
    // between 1.1.1970 and the timestamp in seconds.
    return static_cast<std::time_t>(
        boost::posix_time::time_duration(
            boost::posix_time::time_from_string(timestamp) - time_t_epoch
        ).total_seconds());
    // TODO: Fix date according timezone (using boost::local_time).
}

static void output_blob(void)
{
    std::cout << "blob\n";
    // TODO: Currently I don't know why import.py uses + 1,
    // that might be to avoid revisions with 0.
    //std::cout << "mark :" << id_revision + 1 << '\n';
    std::cout << "mark :" << id_revision << '\n';
    std::cout << "data " << text.size() << '\n';
    std::cout << text << '\n';
}

// Is called whenever a revision tag was closed.
static void newRevision(void)
{
    output_blob();
    std::time_t date = time_t_from_timestamp();
    if( ! tempfilename.empty() ) {
        std::streampos pos = writeString(buildCommitString(date));
        revisionPositions.insert(
            std::pair<std::time_t, std::streampos>(date, pos));
    }
    else
        revisions.insert(
            std::pair<std::time_t, std::string>(date, buildCommitString(date)));
    ++revisions_read;
}

static void showStats(void)
{
    unsigned long rev_now(revisions_read + ignoredRevisions);
    std::cerr << "Revisions read: " << rev_now;
    if( revisions_total && rev_now ) {
        std::cerr << '/' << revisions_total;
        std::cerr << " (ETA "
            << boost::posix_time::to_simple_string(
                (boost::posix_time::second_clock::local_time() - time_start)
                * (double)(revisions_total-rev_now)/rev_now )
            << ')' << std::endl;
    }
    else
        std::cerr << std::endl;
}

// Callbacks for expat

static void XMLCALL startElement(void *, const char *name, const char **)
{
    actualValue.clear();
    MapElementsKeyString::const_iterator ei=mapElementNames.find(name);
    if( ei != mapElementNames.end()) {
        //std::cerr << "Element: '" << name << '\'' << std::endl;
        elementStack.push(ei->second);
        if( elementStack.size() == 3 && ei->second == Element_revision ) {
            comment.clear();
            ip.clear();
            text.clear();
            timestamp.clear();
            username.clear();
            is_minor = false;
            is_del = false;
        }
    }
    else {
        //std::cerr << "Unknown element: '" << name << '\'' << std::endl;
        elementStack.push(Element_unknown);
    }
}

static void XMLCALL endElement(void *, const char *name)
{
    switch(elementStack.top()) {
        case Element_comment:
            if( elementStack.size() == 4 ) // below revision
                comment.swap(actualValue);
            break;
        case Element_id:
            if( elementStack.size() == 4 ) // below revision
                id_revision.swap(actualValue);
            else if( elementStack.size() == 5 ) // below contributor
                id_contributor.swap(actualValue);
            else if( elementStack.size() == 3 ) // below page
                id_page.swap(actualValue);
           break;
        case Element_ip:
            if( elementStack.size() == 5  || elementStack.size() == 6 ) // below contributor or below username
                ip.swap(actualValue);
            break;
        case Element_minor:
            if( elementStack.size() == 4 ) // below revision
                is_minor = true;
            break;
        case Element_text:
            if( elementStack.size() == 4 ) // below revision
                text.swap(actualValue);
            break;
        case Element_revision:
            if( elementStack.size() == 3 ) { // below page
                if( ! ignorePage )
                    newRevision();
                else
                    ++ignoredRevisions;
            }
            break;
        case Element_timestamp:
            if( elementStack.size() == 4 ) // below revision
                timestamp.swap(actualValue);
            break;
        case Element_title:
            if( elementStack.size() == 3 ) { // below page
                showStats();
                title.swap(actualValue);
                std::cerr << "Processing page " << title << std::endl;
                ignorePage = false;
                size_t colon = title.find(':');
                if( colon != std::string::npos ) {
                    title_ns = title.substr(0, colon);
                    if( ns_blacklist.find(title_ns) != ns_blacklist.end() ) {
                        ignorePage = true;
                        ++ignoredPages;
                    }
                    // TODO: We should check if this is a namespace
                    // (which would require to read the namespaces).
                    title.erase(0, colon+1);
                }
                else
                    title_ns.clear();
                if( ignorePage )
                    std::cerr << "(blacklisted => ignored)" << std::endl;
            }
            break;
        case Element_username:
            if( elementStack.size() == 5 ) // below contributor
                username.swap(actualValue);
            break;
       default:
            break;
    }
    elementStack.pop();
}

static void XMLCALL characterHandler(void *, const char *txt, int txtlen)
{
    actualValue += std::string(txt, txtlen);
}

static void printMemInfo(void)
{
    // See http://www.gnu.org/software/libc/manual/html_node/Statistics-of-Malloc.html
    // and have a look at /usr/include/malloc.h.
    struct mallinfo info = mallinfo();
    // This is the total size of memory allocated with sbrk (not mmaped) by malloc
    // plus size of memory allocated with mmap, both in bytes.
    std::cerr << "Allocated 1: " << info.arena+info.hblkhd << " Bytes" << std::endl;
    // This is the total size of memory occupied by chunks handed out by malloc and
    // mmap.
    std::cerr << "Allocated 2: " << info.uordblks+info.hblkhd << " Bytes" << std::endl;
}

static std::string output_commit(const std::string& str,
    const std::string& from)
{
    std::cout << "commit refs/heads/master\n";
    // Get the start of the line beginning with M 100644 :mark.
    // Used to insert From and to get the mark.
    size_t m_start = str.rfind('\n')+1;
    size_t mark_start = str.find(':', m_start)+1;
    size_t mark_end = str.find(' ', mark_start);
    assert( mark_end != std::string::npos );
    // This moves the mark from the blob to the commit.
    std::cout << "mark :" << str.substr(mark_start, mark_end-mark_start) << '\n';
    if( !from.empty() ) {
        std::cout << str.substr(0, m_start);
        std::cout << "from :" << from << '\n';
        std::cout << str.substr(m_start) << '\n';
    }
    else
        std::cout << str << '\n';
    return str.substr(mark_start, mark_end-mark_start);
}

static void readBlacklist(void)
{
    std::ifstream blist;
    try {
        blist.open(blacklist);
    }
    catch (std::exception& e) {
        // e.what() offers only cryptic errors here
        std::cerr << "ERROR: Can't open file '" << blacklist << "'!" << std::endl;
        return;
    }
    std::string s;
    while(std::getline(blist, s)) {
        if( s.empty() || s[0] == '#' )
            continue;
        ns_blacklist.insert(s);
    }
    blist.close();
}

int main(int argc, char** argv)
{
    std::cerr << std::endl << "wp2git version " VERSION << std::endl;
    std::cerr << "(c) 2009 Alexander Holler" << std::endl << std::endl;

    int rc=config(argc, argv);
    if(rc)
        return rc;

    if( ! blacklist.empty() )
        readBlacklist();


    std::cerr << "Step 1: Creating blobs." << std::endl;

    time_start = boost::posix_time::second_clock::local_time();

    // Initialize the parser
    initMap();
    struct XML_ParserStruct* parser(XML_ParserCreate(NULL));
    assert(parser);
    XML_SetElementHandler(parser, startElement, endElement);
    XML_SetCharacterDataHandler(parser, characterHandler);

    // Open the file with bzip-decompressor or stdin
    std::istream* infile;
    std::ifstream file(filename, std::ios_base::in | std::ios_base::binary);
    boost::iostreams::filtering_streambuf<boost::iostreams::input> in;
    if( filename.empty() )
        infile = &std::cin;
    else {
        // Create the input stream
        in.push(boost::iostreams::bzip2_decompressor());
        in.push(file);
        infile = new std::istream(&in);
    }

    // Open the temporary file
    if( ! tempfilename.empty() ) {
        tfile.exceptions( std::fstream::failbit | std::fstream::badbit );
        //tfile.exceptions( std::ifstream::eofbit | std::fstream::failbit | std::fstream::badbit );
        try {
            tfile.open(tempfilename,
                std::fstream::binary | std::fstream::in | std::fstream::out | std::fstream::trunc);
        }
        catch (std::exception& e) {
            // e.what() offers only cryptic errors here
            std::cerr << "ERROR: Can't open file '" << tempfilename << "'!" << std::endl;
            return 2;
        }
    }

    // Read, parse and output blobs.
    while( *infile && revisions_read < max_revisions ) {
        void* parseBuffer(XML_GetBuffer(parser, BUFFER_SIZE));
        infile->read((char*)parseBuffer, BUFFER_SIZE);
        if (XML_ParseBuffer(parser, infile->gcount(), 0) == XML_STATUS_ERROR) {
            std::cerr << XML_ErrorString(XML_GetErrorCode(parser)) << std::endl;
            return 1;
        }
        if(revisions_read >= max_revisions)
            break; // This will create some more blobs, but we don't care.
    }

    // Output commits.

    if( ! revisions_read ) {
        std::cerr << "No revisions read!" << std::endl;
        exit(0);
    }

    printMemInfo();

    std::cerr << "Step 2: Writing " << std::min(revisions_read, max_revisions)
        << " commits." << std::endl;

    if( ! tempfilename.empty() ) {
        RevisionPositions::iterator i = revisionPositions.begin();
        std::string from(output_commit(readString(i->second), ""));
        revisionPositions.erase(i++);
        RevisionPositions::const_iterator end = revisionPositions.end();
        for( size_t count = 1 ; i != end && count < max_revisions; ++count ) {
            from = output_commit(readString(i->second), from);
            revisionPositions.erase(i++);
        }
        tfile.close();
        // TODO: unlink tfile
    }
    else {
        Revisions::iterator i = revisions.begin();
        std::string from(output_commit(i->second, ""));
        revisions.erase(i++);
        Revisions::const_iterator end = revisions.end();
        for( size_t count = 1 ; i != end && count < max_revisions; ++count ) {
            from = output_commit(i->second, from);
            revisions.erase(i++);
        }
    }

    std::cerr << "Time needed: " << boost::posix_time::to_simple_string(
        boost::posix_time::second_clock::local_time() - time_start) << std::endl;

    printMemInfo();

    std::cerr << "Processed " << std::min(revisions_read, max_revisions)
        << " revisions." << std::endl;
    if( ignoredPages )
        std::cerr << "Ignored " << ignoredPages << " blacklisted pages (" << ignoredRevisions
            << " revisions)." << std::endl;
    // Let the libc perform all the cleanup and just quit.
    return 0;
}
