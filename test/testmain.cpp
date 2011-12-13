/**
 * @file
 * Main program for running unit tests.
 */

#ifndef __CL_ENABLE_EXCEPTIONS
# define __CL_ENABLE_EXCEPTIONS 1
#endif

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <iostream>
#include <cppunit/Test.h>
#include <cppunit/TestCase.h>
#include <cppunit/TextTestRunner.h>
#include <cppunit/CompilerOutputter.h>
#include <cppunit/BriefTestProgressListener.h>
#include <cppunit/TestResult.h>
#include <cppunit/extensions/TestFactoryRegistry.h>
#include <boost/program_options.hpp>
#include <string>
#include <stdexcept>
#include <typeinfo>
#include "../src/clh.h"

using namespace std;
namespace po = boost::program_options;

namespace TestSet
{
string perBuild()   { return "build"; }
string perCommit()  { return "commit"; }
string perNightly() { return "nightly"; }
};

static po::variables_map g_vm;

const po::variables_map &testGetOptions()
{
    return g_vm;
}

static void listTests(CppUnit::Test *root, string path)
{
    if (!path.empty())
        path += '/';
    path += root->getName();

    cout << path << '\n';
    for (int i = 0; i < root->getChildTestCount(); i++)
    {
        CppUnit::Test *sub = root->getChildTestAt(i);
        listTests(sub, path);
    }
}

static po::variables_map processOptions(int argc, const char **argv)
{
    po::options_description desc("Options");
    desc.add_options()
        ("help",                                      "Show help");

    po::options_description test("Test options");
    test.add_options()
        ("test", po::value<string>()->default_value("build"), "Choose test")
        ("list",                                      "List all tests")
        ("verbose,v",                                 "Show result of each test as it runs");
    desc.add(test);

    po::options_description cl("OpenCL options");
    CLH::addOptions(cl);
    desc.add(cl);

    try
    {
        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv)
                  .style(po::command_line_style::default_style & ~po::command_line_style::allow_guessing)
                  .options(desc)
                  .run(), vm);
        po::notify(vm);

        if (vm.count("help"))
        {
            cout << desc << '\n';
            exit(0);
        }
        return vm;
    }
    catch (po::error &e)
    {
        cerr << e.what() << "\n\n" << desc << '\n';
        exit(1);
    }
}

int main(int argc, const char **argv)

{
    try
    {
        g_vm = processOptions(argc, argv);

        CppUnit::TestSuite *rootSuite = new CppUnit::TestSuite("All tests");
        CppUnit::TestFactoryRegistry::getRegistry().addTestToSuite(rootSuite);
        rootSuite->addTest(CppUnit::TestFactoryRegistry::getRegistry(TestSet::perCommit()).makeTest());
        rootSuite->addTest(CppUnit::TestFactoryRegistry::getRegistry(TestSet::perBuild()).makeTest());
        rootSuite->addTest(CppUnit::TestFactoryRegistry::getRegistry(TestSet::perNightly()).makeTest());

        if (g_vm.count("list"))
        {
            listTests(rootSuite, "");
            return 0;
        }
        string path = g_vm["test"].as<string>();

        CppUnit::BriefTestProgressListener listener;
        CppUnit::TextTestRunner runner;
        runner.addTest(rootSuite);
        runner.setOutputter(new CppUnit::CompilerOutputter(&runner.result(), std::cerr));
        if (g_vm.count("verbose"))
            runner.eventManager().addListener(&listener);
        bool success = runner.run(path, false, true, false);
        return success ? 0 : 1;
    }
    catch (invalid_argument &e)
    {
        cerr << "\nERROR: " << e.what() << "\n";
        return 2;
    }
}
