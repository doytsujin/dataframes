#define BOOST_TEST_MODULE CsvTests
#include <boost/test/included/unit_test.hpp>
#include <chrono>

#include "csv.h"
#include "IO.h"

#pragma comment(lib, "DataframeHelper.lib")

void testFieldParser(std::string input, std::string expectedContent, int expectedPosition)
{
	auto itr = input.data();
	auto nsv = parseField(itr, itr + input.length(), ',', '\n', '"');

	BOOST_TEST_CONTEXT("Parsing `" << input << "`")
	{
		BOOST_CHECK_EQUAL(nsv.str(), expectedContent);
		BOOST_CHECK_EQUAL(std::distance(input.data(), itr), expectedPosition);
	}
}

BOOST_AUTO_TEST_CASE(ParseField)
{
	testFieldParser("foo", "foo", 3);
	testFieldParser("foo,bar", "foo", 3);
	testFieldParser(",bar", "", 0);
	testFieldParser(R"("foo")", "foo", 5);
	testFieldParser(R"("fo""o")", "fo\"o", 7);
	testFieldParser(R"("fo""o,"",bar")", "fo\"o,\",bar", 14);
	std::string buffer = "foo,bar";
}

void testRecordParser(std::string input, std::vector<std::string> expectedContents)
{
	auto itr = input.data();
	auto fields = parseRecord(itr, itr + input.length(), ',', '\n', '"');

	BOOST_TEST_CONTEXT("Parsing `" << input << "`")
	{
		BOOST_REQUIRE_EQUAL(fields.size(), expectedContents.size());
		for(int i = 0; i < expectedContents.size(); i++)
			BOOST_CHECK_EQUAL(fields.at(i).str(), expectedContents.at(i));
	}
}

BOOST_AUTO_TEST_CASE(ParseRecord)
{
	testRecordParser("foo,bar,b az", {"foo", "bar", "b az"});
	testRecordParser("foo,bar,b az\n\n\n", {"foo", "bar", "b az"});
	testRecordParser("foo", {"foo"});
	testRecordParser("foo\nbar", {"foo"});
	testRecordParser("\nfoo", {""});
	testRecordParser("\n\n\n", {""});
	testRecordParser(R"("f""o,o")", {R"(f"o,o)"});
}

void testCsvParser(std::string input, std::vector<std::vector<std::string>> expectedContents)
{
	auto itr = input.data();
	auto rows = parseCsvTable(itr, itr + input.length(), ',', '\n', '"');

	BOOST_TEST_CONTEXT("Parsing `" << input << "`")
	{
		BOOST_REQUIRE_EQUAL(rows.size(), expectedContents.size());
		for(int i = 0; i < expectedContents.size(); i++)
		{
			BOOST_TEST_CONTEXT("row " << i)
			{
				auto &readRow = rows.at(i);
				auto &expectedRow = expectedContents.at(i);
				BOOST_REQUIRE_EQUAL(readRow.size(), expectedRow.size());
				for(int j = 0; j < readRow.size(); j++)
					BOOST_CHECK_EQUAL(readRow.at(j).str(), expectedRow.at(j));
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(ParseCsv)
{
	testCsvParser("foo\nbar\nbaz", {{"foo"}, {"bar"}, {"baz"}});
}

BOOST_AUTO_TEST_CASE(ParseFile)
{
	auto path = R"(F:\dev\Dataframes\data\simple_empty.csv)";
	auto csv = parseCsvFile(path);
	auto table = csvToArrowTable(csv, TakeFirstRowAsHeaders{}, {});
}

template<typename F, typename ...Args>
static auto duration(F&& func, Args&&... args)
{
	const auto start = std::chrono::steady_clock::now();
	std::invoke(std::forward<F>(func), std::forward<Args>(args)...);
	return std::chrono::steady_clock::now() - start;
}

template<typename F, typename ...Args>
static auto measure(std::string text, F&& func, Args&&... args)
{
	const auto t = duration(std::forward<F>(func), std::forward<Args>(args)...);
	std::cout << text << " took " << std::chrono::duration_cast<std::chrono::milliseconds>(t).count() << " ms" << std::endl;
	return t;
}

std::string get_file_contents(const char *filename)
{
	std::ifstream in(filename, std::ios::in);
	if (in)
	{
		std::string contents;
		in.seekg(0, std::ios::end);
		contents.resize(in.tellg());
		in.seekg(0, std::ios::beg);
		in.read(&contents[0], contents.size());
		in.close();
		return(contents);
	}
	throw(errno);
}

BOOST_AUTO_TEST_CASE(ParseBigFile)
{
 	const auto path = R"(E:/hmda_lar-florida.csv)";
// 	for(int i  = 0; i < 20; i++)
// 	{
// 		measure("load big file contents1", getFileContents, path);
// 		measure("load big file contents2", get_file_contents, path);
// 	}

	measure("big file", [&]
	{
		auto csv = parseCsvFile(path);
		auto table = csvToArrowTable(csv, TakeFirstRowAsHeaders{}, {});
	});
}