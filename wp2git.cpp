// (c) 2009 Alexander Holler
// See the file COPYING for copying permission.
//
// No funny objects created, this is a very small and dump programm
// and we are just using globals.
//
// I'm believing in KISS (keep it stupid, simple).

#include <fstream>
#include <iostream>
#include <string>
#include <stack>
#include <tr1/unordered_map> // You will need a gcc >= 3.x

#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/filter/bzip2.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options/cmdline.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/program_options/options_description.hpp>

#include <expat.h>

#include "version.h"

#define BUFFER_SIZE 1024*1024

// Stuff we are reading and feeding to git.
static std::string comment;
static std::string id;
static std::string ip;
static std::string text;
static std::string timestamp;
static std::string title;
static std::string username;

// Options.
static std::string filename;
static std::string committer("wp2git <wp2git@localhost.localdomain>");
static unsigned deepness(3);
static bool wikitime(false);
static std::string programname;

// Miscellaneous
unsigned long pagecount(0);
unsigned long revisioncount(0);

enum Element {
    Element_unknown,
    Element_comment,
    Element_id,
    Element_ip,
    Element_restrictions,
    Element_revision,
    Element_text,
    Element_timestamp,
    Element_title,
    Element_username,
    // The stuff below doesn't interest us.
/*
    Element_contributor,
    Element_mediawiki,
    Element_minor,
    Element_namespace,
    Element_page,
*/
};

typedef std::tr1::unordered_map<std::string, Element> MapElementsKeyString;
static MapElementsKeyString mapElementNames;

static std::stack<Element> elementStack;

static std::string actualValue;

// The actual code starts here.

static void initMap(void)
{
    assert( mapElementNames.empty() );
    mapElementNames["comment"] = Element_comment;
    mapElementNames["id"] = Element_id;
    mapElementNames["ip"] = Element_ip;
    mapElementNames["restrictions"] = Element_restrictions;
    mapElementNames["revision"] = Element_revision;
    mapElementNames["text"] = Element_text;
    mapElementNames["timestamp"] = Element_timestamp;
    mapElementNames["title"] = Element_title;
    mapElementNames["username"] = Element_username;
    // The stuff below doesn't interest us.
/*
    mapElementNames["contributor"] = Element_contributor;
    mapElementNames["mediawiki"]=Element_mediawiki;
    mapElementNames["minor"] = Element_minor;
    mapElementNames["namespace"] = Element_namespace;
    mapElementNames["page"] = Element_page;
*/
}

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
        ("committer,c", boost::program_options::value<std::string>(&committer),
            std::string("git \"Committer\" used while doing the commits (default \"" + committer + "\")").c_str())
        ("deepness,d", boost::program_options::value<unsigned>(&deepness),
            "The deepness of the result directory structure (default 3)")
        ("wikitime,w", boost::program_options::bool_switch(&wikitime),
            "If true, the commit time will be set to the revision creation, not the current system time (default false)")
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
    if( vm.count("mediawiki-export-bz2") != 1 ) {
         printHelp(programname, desc);
        return 3;
    }
    filename = vm["mediawiki-export-bz2"].as< std::vector<std::string> >()[0];
    return 0;
}

// Is called whenever a revision tag was closed.
static void newRevision(void)
{
    std::cout << "blob\n";
    unsigned idPlusOne(boost::lexical_cast<unsigned>(id)+1);
    std::cout << "mark :" << idPlusOne << '\n';
    std::cout << "data " << text.size() << '\n';
    std::cout << text << '\n';
    ++revisioncount;
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
            id.clear();
            ip.clear();
            text.clear();
            timestamp.clear();
            username.clear();
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
                id.swap(actualValue);
            break;
        case Element_ip:
            if( elementStack.size() == 6 ) // below username
                ip.swap(actualValue);
            break;
        case Element_text:
            if( elementStack.size() == 4 ) // below revision
                text.swap(actualValue);
            break;
       case Element_revision:
            if( elementStack.size() == 3 ) // below page
               newRevision();
            break;
        case Element_timestamp:
            if( elementStack.size() == 4 ) // below revision
                timestamp.swap(actualValue);
            break;
        case Element_title:
            if( elementStack.size() == 3 ) { // below page
                title.swap(actualValue);
                std::cerr << "Processing page " << title << std::endl;
                ++pagecount;
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

// The small main

int main(int argc, char** argv)
{
    std::cerr << std::endl << "wp2git version " VERSION << std::endl;
    std::cerr << "(c) 2009 Alexander Holler" << std::endl << std::endl;

    int rc=config(argc, argv);
    if(rc)
        return rc;

    std::cerr << "Step 1: Creating blobs." << std::endl;

    // Initialize the parser
    initMap();
    struct XML_ParserStruct* parser(XML_ParserCreate(NULL));
    assert(parser);
    XML_SetElementHandler(parser, startElement, endElement);
    XML_SetCharacterDataHandler(parser, characterHandler);

    // Create the input stream
    std::ifstream file(filename, std::ios_base::in | std::ios_base::binary);
    boost::iostreams::filtering_streambuf<boost::iostreams::input> in;
    in.push(boost::iostreams::bzip2_decompressor());
    in.push(file);
    std::istream ins(&in);

    // Read and parse
    while( ins ) {
        void* parseBuffer(XML_GetBuffer(parser, BUFFER_SIZE));
        ins.read((char*)parseBuffer, BUFFER_SIZE);
        if (XML_ParseBuffer(parser, ins.gcount(), 0) == XML_STATUS_ERROR) {
            std::cerr << XML_ErrorString(XML_GetErrorCode(parser)) << std::endl;
            return 1;
        }
    }

    std::cerr << "Processed " << pagecount << " pages (" << revisioncount << " revisions)." << std::endl;

    // Let the libc perform all the cleanup and just quit.
    return 0;
}
