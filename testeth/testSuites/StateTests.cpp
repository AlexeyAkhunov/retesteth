/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file StateTests.cpp
 * @author Dimitry Khokhlov <dimitry@ethereum.org>
 * @date 2016
 * General State Tests parser.
 */

#include <boost/filesystem/operations.hpp>
#include <boost/test/unit_test.hpp>

#include <devcore/CommonIO.h>
#include <testeth/Options.h>
#include <testeth/TestOutputHelper.h>
#include <testeth/TestHelper.h>
#include <testeth/TestSuite.h>
#include <testeth/DataObject.h>
#include <testeth/testSuites/StateTests.h>

#include <testeth/ethObjects/genesis.h>
#include <testeth/ethObjects/state.h>
#include <testeth/ethObjects/transaction.h>

#include <testeth/RPCSession.h>

using namespace std;
using namespace dev;
namespace fs = boost::filesystem;

namespace test {

struct transactionInfo
{
	transactionInfo(size_t _dataInd, size_t _gasInd, size_t _valueInd, DataObject const& _transaction) :
		gasInd(_gasInd), dataInd(_dataInd), valueInd(_valueInd), executed(false), transaction(_transaction)
		{}
	size_t gasInd;
	size_t dataInd;
	size_t valueInd;
	bool executed;
	test::transaction transaction;
};

std::vector<transactionInfo> parseGeneralTransaction(DataObject const& _generalTransaction)
{
	std::vector<transactionInfo> ret;
	test::requireJsonFields(_generalTransaction, "transaction", {
		{"data", {{DataType::Array}} },
		{"gasLimit", {{DataType::Array}} },
		{"gasPrice", {{DataType::String}} },
		{"nonce", {{DataType::String}} },
		{"secretKey", {{DataType::String}} },
		{"to", {{DataType::String}} },
		{"value", {{DataType::Array}} }
	});

	for (size_t dataInd = 0; dataInd < _generalTransaction.at("data").getSubObjects().size(); dataInd++)
	{
		for (size_t gasInd = 0; gasInd < _generalTransaction.at("gasLimit").getSubObjects().size(); gasInd++)
		{
			for (size_t valueInd = 0; valueInd < _generalTransaction.at("value").getSubObjects().size(); valueInd++)
			{
				DataObject singleTransaction(DataType::Object);
				DataObject data("data", _generalTransaction.at("data").getSubObjects().at(dataInd).asString());
				DataObject gas("gasLimit", _generalTransaction.at("gasLimit").getSubObjects().at(gasInd).asString());
				DataObject value("value", _generalTransaction.at("value").getSubObjects().at(valueInd).asString());

				singleTransaction.addSubObject(data);
				singleTransaction.addSubObject(gas);
				singleTransaction.addSubObject(_generalTransaction.at("gasPrice"));
				singleTransaction.addSubObject(_generalTransaction.at("nonce"));
				singleTransaction.addSubObject(_generalTransaction.at("secretKey"));
				singleTransaction.addSubObject(_generalTransaction.at("to"));
				singleTransaction.addSubObject(value);
				transactionInfo info(dataInd, gasInd, valueInd, singleTransaction);
				ret.push_back(info);
			}
		}
	}
	return ret;
}

void parseJsonIntValueIntoSet(DataObject const& _json, set<int>& _out)
{
	if (_json.type() == DataType::Array)
	{
		for (auto const& val: _json.getSubObjects())
			_out.emplace(val.asInt());
	}
	else if (_json.type() == DataType::Integer)
		_out.emplace(_json.asInt());
}

void FillTest()
{

}

void RunTest(DataObject const& _testFile)
{
	test::state aState(_testFile.at("pre"));
	test::genesis aTestGenesis(_testFile.at("env"), aState);
	std::vector<transactionInfo> transactions = parseGeneralTransaction(_testFile.at("transaction"));
	BOOST_REQUIRE_MESSAGE(_testFile.count("post") > 0, TestOutputHelper::get().testName() + " post not set!");

	RPCSession& session = RPCSession::instance("/home/wins/.ethereum/geth.ipc");

	DataObject genesis;
	genesis["version"] = "1";
	genesis.addSubObject("genesis", aTestGenesis.getData());
	genesis.addSubObject("state", aState.getData());
	genesis["params"]["miningMethod"] = "NoProof";
	genesis["params"]["blockReward"] = "0x00";
	genesis["genesis"]["timestamp"] = "0x00";	//Set Genesis tstmp to 0. the actual timestamp specified in env section is a timestamp of the first block.

	// read post state results
	for (auto const& post: _testFile.at("post").getSubObjects())
	{
		if (!Options::get().singleTestNet.empty() && Options::get().singleTestNet != post.getKey())
			continue;

		BOOST_REQUIRE_MESSAGE(getNetworks().count(post.getKey()), "Unknown network in the post section of the test! " + TestOutputHelper::get().testName() + " network: " + post.getKey());
		genesis["params"]["forkRules"] = post.getKey();

		session.test_setChainParams(genesis.asJson());

		// read all results for a specific fork
		for (auto const& results: post.getSubObjects())
		{
			requireJsonFields(results, "post", {
				{"hash", {{DataType::String}} },
				{"logs", {{DataType::String}} },
				{"indexes", {{DataType::Object}} }
			});
			requireJsonFields(results.at("indexes"), "indexes", {
				{"data", {{DataType::Array}, {DataType::Integer}} },
				{"gas", {{DataType::Array}, {DataType::Integer}} },
				{"value", {{DataType::Array}, {DataType::Integer}} }
			});

			set<int> _dataIndexes;
			parseJsonIntValueIntoSet(results.at("indexes").at("data"), _dataIndexes);
			set<int> _gasIndexes;
			parseJsonIntValueIntoSet(results.at("indexes").at("gas"), _gasIndexes);
			set<int> _valueIndexes;
			parseJsonIntValueIntoSet(results.at("indexes").at("value"), _valueIndexes);

			// look for a transaction with this indexes and execute it on a client
			for (auto& tr: transactions)
			{
				bool blockMined = false;
				if ((_dataIndexes.count(tr.dataInd) || _dataIndexes.count(-1)) &&
					(_gasIndexes.count(tr.gasInd) || _gasIndexes.count(-1)) &&
					(_valueIndexes.count(tr.valueInd) || _valueIndexes.count(-1)))
				{
					u256 a(aTestGenesis.getData().at("timestamp").asString());
					session.test_modifyTimestamp(a.convert_to<size_t>());
					session.test_addTransaction(tr.transaction.getData().asJson());
					session.test_mineBlocks(1);
					tr.executed = true;
					blockMined = true;
					string fullPost;
					string postHash = session.test_getPostState("{ \"version\" : \"1\" }");
					if (postHash != results.at("hash").asString())
						fullPost = session.test_getPostState("{ \"version\" : \"2\" }");
					BOOST_CHECK_MESSAGE(postHash == results.at("hash").asString(),
						"Error at " + TestOutputHelper::get().testName() + ", fork: " + post.getKey() + ", hash mismatch: " + postHash + ", expected: " + results.at("hash").asString()
						 + "\nState Dump: " + fullPost);
				}
				if (blockMined)
					session.test_rewindToBlock(0);
			}
		}

		for (auto const& tr: transactions)
		{
			BOOST_REQUIRE_MESSAGE(tr.executed == true, "A transaction was specified, but there is no execution results in a test! Transaction: dInd="
			+ toString(tr.dataInd) + " gInd=" + toString(tr.gasInd) + " vInd=" + toString(tr.valueInd));
		}
	}
}

DataObject StateTestSuite::doTests(DataObject const& _input, bool _fillin) const
{
	BOOST_REQUIRE_MESSAGE(_input.type() == DataType::Object,
		TestOutputHelper::get().get().testFile().string() + " A GeneralStateTest file should contain an object.");
	BOOST_REQUIRE_MESSAGE(!_fillin || _input.getSubObjects().size() == 1,
		TestOutputHelper::get().testFile().string() + " A GeneralStateTest filler should contain only one test.");
	DataObject v(DataType::Object);

	for (auto& i: _input.getSubObjects())
	{
		string const testname = i.getKey();
		BOOST_REQUIRE_MESSAGE(i.type() == DataType::Object,
			TestOutputHelper::get().testFile().string() + " should contain an object under a test name.");
		DataObject const& inputTest = i;
		DataObject outputTest(DataType::Object);
		outputTest.setKey(testname);

		if (_fillin && !TestOutputHelper::get().testFile().empty())
			BOOST_REQUIRE_MESSAGE(testname + "Filler" == TestOutputHelper::get().testFile().stem().string(),
				TestOutputHelper::get().testFile().string() + " contains a test with a different name '" + testname + "'");

		if (!TestOutputHelper::get().checkTest(testname))
			continue;

		BOOST_REQUIRE_MESSAGE(inputTest.count("env") > 0, testname + " env not set!");
		BOOST_REQUIRE_MESSAGE(inputTest.count("pre") > 0, testname + " pre not set!");
		BOOST_REQUIRE_MESSAGE(inputTest.count("transaction") > 0, testname + " transaction not set!");

		if (_fillin)
			FillTest();
		else
			RunTest(inputTest);
	}
	return v;
}

fs::path StateTestSuite::suiteFolder() const
{
	return "GeneralStateTests";
}

fs::path StateTestSuite::suiteFillerFolder() const
{
	return "GeneralStateTestsFiller";
}

}// Namespace Close

class GeneralTestFixture
{
public:
	GeneralTestFixture()
	{
		test::StateTestSuite suite;
		string casename = boost::unit_test::framework::current_test_case().p_name;
		if (casename == "stQuadraticComplexityTest" && !test::Options::get().all)
		{
			std::cout << "Skipping " << casename << " because --all option is not specified.\n";
			return;
		}
		suite.runAllTestsInFolder(casename);
	}
};

BOOST_FIXTURE_TEST_SUITE(GeneralStateTests, GeneralTestFixture)

//Frontier Tests
BOOST_AUTO_TEST_CASE(stCallCodes){}
BOOST_AUTO_TEST_CASE(stCallCreateCallCodeTest){}
BOOST_AUTO_TEST_CASE(stExample){}
BOOST_AUTO_TEST_CASE(stInitCodeTest){}
BOOST_AUTO_TEST_CASE(stLogTests){}
BOOST_AUTO_TEST_CASE(stMemoryTest){}
BOOST_AUTO_TEST_CASE(stPreCompiledContracts){}
BOOST_AUTO_TEST_CASE(stPreCompiledContracts2){}
BOOST_AUTO_TEST_CASE(stRandom){}
BOOST_AUTO_TEST_CASE(stRandom2){}
BOOST_AUTO_TEST_CASE(stRecursiveCreate){}
BOOST_AUTO_TEST_CASE(stRefundTest){}
BOOST_AUTO_TEST_CASE(stSolidityTest){}
BOOST_AUTO_TEST_CASE(stSpecialTest){}
BOOST_AUTO_TEST_CASE(stSystemOperationsTest){}
BOOST_AUTO_TEST_CASE(stTransactionTest){}
BOOST_AUTO_TEST_CASE(stTransitionTest){}
BOOST_AUTO_TEST_CASE(stWalletTest){}

//Homestead Tests
BOOST_AUTO_TEST_CASE(stCallDelegateCodesCallCodeHomestead){}
BOOST_AUTO_TEST_CASE(stCallDelegateCodesHomestead){}
BOOST_AUTO_TEST_CASE(stHomesteadSpecific){}
BOOST_AUTO_TEST_CASE(stDelegatecallTestHomestead){}

//EIP150 Tests
BOOST_AUTO_TEST_CASE(stChangedEIP150){}
BOOST_AUTO_TEST_CASE(stEIP150singleCodeGasPrices){}
BOOST_AUTO_TEST_CASE(stMemExpandingEIP150Calls){}
BOOST_AUTO_TEST_CASE(stEIP150Specific){}

//EIP158 Tests
BOOST_AUTO_TEST_CASE(stEIP158Specific){}
BOOST_AUTO_TEST_CASE(stNonZeroCallsTest){}
BOOST_AUTO_TEST_CASE(stZeroCallsTest){}
BOOST_AUTO_TEST_CASE(stZeroCallsRevert){}
BOOST_AUTO_TEST_CASE(stCodeSizeLimit){}
BOOST_AUTO_TEST_CASE(stCreateTest){}
BOOST_AUTO_TEST_CASE(stRevertTest){}

//Metropolis Tests
BOOST_AUTO_TEST_CASE(stStackTests){}
BOOST_AUTO_TEST_CASE(stStaticCall){}
BOOST_AUTO_TEST_CASE(stReturnDataTest){}
BOOST_AUTO_TEST_CASE(stZeroKnowledge){}
BOOST_AUTO_TEST_CASE(stZeroKnowledge2){}
BOOST_AUTO_TEST_CASE(stCodeCopyTest){}
BOOST_AUTO_TEST_CASE(stBugs){}

//Stress Tests
BOOST_AUTO_TEST_CASE(stAttackTest){}
BOOST_AUTO_TEST_CASE(stMemoryStressTest){}
BOOST_AUTO_TEST_CASE(stQuadraticComplexityTest){}

//Invalid Opcode Tests
BOOST_AUTO_TEST_CASE(stBadOpcode){}

//New Tests
BOOST_AUTO_TEST_CASE(stArgsZeroOneBalance){}
BOOST_AUTO_TEST_CASE(stEWASMTests){}
BOOST_AUTO_TEST_SUITE_END()

