// Copyright (c) 2012-2017, The CryptoNote developers, The Bytecoin developers
// Copyright (c) 2018, Leviar developers
//
// This file is part of Bytecoin.
//
// Bytecoin is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Bytecoin is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with Bytecoin.  If not, see <http://www.gnu.org/licenses/>.

#include "MiniWallet.h"

#include <ctime>
#include <fstream>
#include <future>
#include <iomanip>
#include <thread>
#include <sstream>

#include <boost/bind.hpp>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include "Common/CommandLine.h"
#include "Common/SignalHandler.h"
#include "Common/StringTools.h"
#include "Common/PathTools.h"
#include "Common/Util.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteProtocol/CryptoNoteProtocolHandler.h"
#include "NodeRpcProxy/NodeRpcProxy.h"

#include "Wallet/WalletRpcServer.h"
#include "WalletLegacy/WalletLegacy.h"
#include "Wallet/LegacyKeysImporter.h"
#include "WalletLegacy/WalletHelper.h"
#include "PasswordContainer.h"

#include "version.h"

#include <Logging/LoggerManager.h>

#if defined(WIN32)
#include <crtdbg.h>
#endif

using namespace CryptoNote;
using namespace Logging;
using Common::JsonValue;

namespace po = boost::program_options;

#define EXTENDED_LOGS_FILE "wallet_details.log"
#undef ERROR

namespace {

const command_line::arg_descriptor<std::string> arg_wallet_file = { "wallet-file", "Use wallet <arg>", "" };
const command_line::arg_descriptor<std::string> arg_generate_new_wallet = { "generate-new-wallet", "Generate new wallet and save it to <arg>", "" };
const command_line::arg_descriptor<std::string> arg_daemon_address = { "daemon-address", "Use daemon instance at <host>:<port>", "" };
const command_line::arg_descriptor<std::string> arg_daemon_host = { "daemon-host", "Use daemon instance at host <arg> instead of localhost", "" };
const command_line::arg_descriptor<std::string> arg_password = { "password", "Wallet password", "", true };
const command_line::arg_descriptor<uint16_t> arg_daemon_port = { "daemon-port", "Use daemon instance at port <arg> instead of 8081", 0 };


bool parseUrlAddress(const std::string& url, std::string& address, uint16_t& port) {
  auto pos = url.find("://");
  size_t addrStart = 0;

  if (pos != std::string::npos) {
    addrStart = pos + 3;
  }

  auto addrEnd = url.find(':', addrStart);

  if (addrEnd != std::string::npos) {
    auto portEnd = url.find('/', addrEnd);
    port = Common::fromString<uint16_t>(url.substr(
      addrEnd + 1, portEnd == std::string::npos ? std::string::npos : portEnd - addrEnd - 1));
  } else {
    addrEnd = url.find('/');
    port = 80;
  }

  address = url.substr(addrStart, addrEnd - addrStart);
  return true;
}

template <typename IterT, typename ValueT = typename IterT::value_type>
class ArgumentReader {
public:

  ArgumentReader(IterT begin, IterT end) :
    m_begin(begin), m_end(end), m_cur(begin) {
  }

  bool eof() const {
    return m_cur == m_end;
  }

  ValueT next() {
    if (eof()) {
      throw std::runtime_error("unexpected end of arguments");
    }

    return *m_cur++;
  }

private:

  IterT m_cur;
  IterT m_begin;
  IterT m_end;
};

struct TransferCommand {
  const CryptoNote::Currency& m_currency;
  size_t fake_outs_count;
  std::vector<CryptoNote::WalletLegacyTransfer> dsts;
  std::vector<uint8_t> extra;
  uint64_t fee;

  TransferCommand(const CryptoNote::Currency& currency) :
    m_currency(currency), fake_outs_count(0), fee(currency.minimumFee()) {
  }

  bool parseArguments(LoggerRef& logger, const std::vector<std::string> &args) {

    ArgumentReader<std::vector<std::string>::const_iterator> ar(args.begin(), args.end());

    try {

      auto mixin_str = ar.next();

      if (!Common::fromString(mixin_str, fake_outs_count)) {
        logger(ERROR, BRIGHT_RED) << "mixin_count should be non-negative integer, got " << mixin_str;
        return false;
      }

      while (!ar.eof()) {

        auto arg = ar.next();

        if (arg.size() && arg[0] == '-') {

          const auto& value = ar.next();

          if (arg == "-p") {
            if (!createTxExtraWithPaymentId(value, extra)) {
              logger(ERROR, BRIGHT_RED) << "payment ID has invalid format: \"" << value << "\", expected 64-character string";
              return false;
            }
          } else if (arg == "-f") {
            bool ok = m_currency.parseAmount(value, fee);
            if (!ok) {
              logger(ERROR, BRIGHT_RED) << "Fee value is invalid: " << value;
              return false;
            }

            if (fee < m_currency.minimumFee()) {
              logger(ERROR, BRIGHT_RED) << "Fee value is less than minimum: " << m_currency.minimumFee();
              return false;
            }
          }
        } else {
          WalletLegacyTransfer destination;
          CryptoNote::TransactionDestinationEntry de;

          if (!m_currency.parseAccountAddressString(arg, de.addr)) {
            Crypto::Hash paymentId;
            if (CryptoNote::parsePaymentId(arg, paymentId)) {
              logger(ERROR, BRIGHT_RED) << "Invalid payment ID usage. Please, use -p <payment_id>. See help for details.";
            } else {
              logger(ERROR, BRIGHT_RED) << "Wrong address: " << arg;
            }

            return false;
          }

          auto value = ar.next();
          bool ok = m_currency.parseAmount(value, de.amount);
          if (!ok || 0 == de.amount) {
            logger(ERROR, BRIGHT_RED) << "amount is wrong: " << arg << ' ' << value <<
              ", expected number from 0 to " << m_currency.formatAmount(std::numeric_limits<uint64_t>::max());
            return false;
          }
          destination.address = arg;
          destination.amount = de.amount;

          dsts.push_back(destination);
        }
      }

      if (dsts.empty()) {
        logger(ERROR, BRIGHT_RED) << "At least one destination address is required";
        return false;
      }
    } catch (const std::exception& e) {
      logger(ERROR, BRIGHT_RED) << e.what();
      return false;
    }

    return true;
  }
};

JsonValue buildLoggerConfiguration(Level level, const std::string& logfile) {
  JsonValue loggerConfiguration(JsonValue::OBJECT);
  loggerConfiguration.insert("globalLevel", static_cast<int64_t>(level));

  JsonValue& cfgLoggers = loggerConfiguration.insert("loggers", JsonValue::ARRAY);

  JsonValue& consoleLogger = cfgLoggers.pushBack(JsonValue::OBJECT);
  consoleLogger.insert("type", "console");
  consoleLogger.insert("level", static_cast<int64_t>(TRACE));
  consoleLogger.insert("pattern", "%D %T %L ");

  JsonValue& fileLogger = cfgLoggers.pushBack(JsonValue::OBJECT);
  fileLogger.insert("type", "file");
  fileLogger.insert("filename", logfile);
  fileLogger.insert("level", static_cast<int64_t>(TRACE));

  return loggerConfiguration;
}

std::error_code initAndLoadWallet(IWalletLegacy& wallet, std::istream& walletFile, const std::string& password) {
  WalletHelper::InitWalletResultObserver initObserver;
  std::future<std::error_code> f_initError = initObserver.initResult.get_future();

  WalletHelper::IWalletRemoveObserverGuard removeGuard(wallet, initObserver);
  wallet.initAndLoad(walletFile, password);
  auto initError = f_initError.get();

  return initError;
}

std::string tryToOpenWalletOrLoadKeysOrThrow(LoggerRef& logger, std::unique_ptr<IWalletLegacy>& wallet, const std::string& walletFile, const std::string& password) {
  std::string keys_file, walletFileName;
  WalletHelper::prepareFileNames(walletFile, keys_file, walletFileName);

  boost::system::error_code ignore;
  bool keysExists = boost::filesystem::exists(keys_file, ignore);
  bool walletExists = boost::filesystem::exists(walletFileName, ignore);
  if (!walletExists && !keysExists && boost::filesystem::exists(walletFile, ignore)) {
    boost::system::error_code renameEc;
    boost::filesystem::rename(walletFile, walletFileName, renameEc);
    if (renameEc) {
      throw std::runtime_error("failed to rename file '" + walletFile + "' to '" + walletFileName + "': " + renameEc.message());
    }

    walletExists = true;
  }

  if (walletExists) {
    logger(INFO) << "Loading wallet...";
    std::ifstream walletFile;
    walletFile.open(walletFileName, std::ios_base::binary | std::ios_base::in);
    if (walletFile.fail()) {
      throw std::runtime_error("error opening wallet file '" + walletFileName + "'");
    }

    auto initError = initAndLoadWallet(*wallet, walletFile, password);

    walletFile.close();
    if (initError) { //bad password, or legacy format
      if (keysExists) {
        std::stringstream ss;
        CryptoNote::importLegacyKeys(keys_file, password, ss);
        boost::filesystem::rename(keys_file, keys_file + ".back");
        boost::filesystem::rename(walletFileName, walletFileName + ".back");

        initError = initAndLoadWallet(*wallet, ss, password);
        if (initError) {
          throw std::runtime_error("failed to load wallet: " + initError.message());
        }

        logger(INFO) << "Storing wallet...";

        try {
          CryptoNote::WalletHelper::storeWallet(*wallet, walletFileName);
        } catch (std::exception& e) {
          logger(ERROR, BRIGHT_RED) << "Failed to store wallet: " << e.what();
          throw std::runtime_error("error saving wallet file '" + walletFileName + "'");
        }

        logger(INFO, BRIGHT_GREEN) << "Stored ok";
        return walletFileName;
      } else { // no keys, wallet error loading
        throw std::runtime_error("can't load wallet file '" + walletFileName + "', check password");
      }
    } else { //new wallet ok 
      return walletFileName;
    }
  } else if (keysExists) { //wallet not exists but keys presented
    std::stringstream ss;
    CryptoNote::importLegacyKeys(keys_file, password, ss);
    boost::filesystem::rename(keys_file, keys_file + ".back");

    WalletHelper::InitWalletResultObserver initObserver;
    std::future<std::error_code> f_initError = initObserver.initResult.get_future();

    WalletHelper::IWalletRemoveObserverGuard removeGuard(*wallet, initObserver);
    wallet->initAndLoad(ss, password);
    auto initError = f_initError.get();

    removeGuard.removeObserver();
    if (initError) {
      throw std::runtime_error("failed to load wallet: " + initError.message());
    }

    logger(INFO) << "Storing wallet...";

    try {
      CryptoNote::WalletHelper::storeWallet(*wallet, walletFileName);
    } catch(std::exception& e) {
      logger(ERROR, BRIGHT_RED) << "Failed to store wallet: " << e.what();
      throw std::runtime_error("error saving wallet file '" + walletFileName + "'");
    }

    logger(INFO, BRIGHT_GREEN) << "Stored ok";
    return walletFileName;
  } else { //no wallet no keys
    throw std::runtime_error("wallet file '" + walletFileName + "' is not found");
  }
}

std::string prepareWalletAddressFilename(const std::string& walletBaseName) {
  return walletBaseName + ".address";
}

bool writeAddressFile(const std::string& addressFilename, const std::string& address) {
  std::ofstream addressFile(addressFilename, std::ios::out | std::ios::trunc | std::ios::binary);
  if (!addressFile.good()) {
    return false;
  }

  addressFile << address;

  return true;
}

class InitWaiter : public CryptoNote::IWalletLegacyObserver {
public:
	InitWaiter() : future(promise.get_future()) {}

	virtual void initCompleted(std::error_code result) override {
		promise.set_value(result);
	}

	std::error_code waitInit() {
		return future.get();
	}
private:
	std::promise<std::error_code> promise;
	std::future<std::error_code> future;
};


class SaveWaiter : public CryptoNote::IWalletLegacyObserver {
public:
	SaveWaiter() : future(promise.get_future()) {}

	virtual void saveCompleted(std::error_code result) override {
		promise.set_value(result);
	}

	std::error_code waitSave() {
		return future.get();
	}

private:
	std::promise<std::error_code> promise;
	std::future<std::error_code> future;
};

} //namespace

mini_wallet::mini_wallet(System::Dispatcher& dispatcher, const CryptoNote::Currency& currency, Logging::LoggerManager& log) :
  m_dispatcher(dispatcher),
  m_daemon_port(0), 
  m_currency(currency), 
  logManager(log),
  logger(log, "miniwallet"),
  m_refresh_progress_reporter(*this), 
  m_initResultPromise(nullptr),
  m_walletSynchronized(false) {
  //
}
//----------------------------------------------------------------------------------------------------
void mini_wallet::handle_command_line(const boost::program_options::variables_map& vm) {
	m_wallet_file_arg = command_line::get_arg(vm, arg_wallet_file);
	m_generate_new = command_line::get_arg(vm, arg_generate_new_wallet);
	m_daemon_address = command_line::get_arg(vm, arg_daemon_address);
	m_daemon_host = command_line::get_arg(vm, arg_daemon_host);
	m_daemon_port = command_line::get_arg(vm, arg_daemon_port);
}
//----------------------------------------------------------------------------------------------------
bool mini_wallet::init(const boost::program_options::variables_map& vm) {
	handle_command_line(vm);
	logManager.setMaxLevel(static_cast<Logging::Level>(2));

  if (!m_daemon_address.empty() && (!m_daemon_host.empty() || 0 != m_daemon_port)) {
    fail_msg_writer() << "you can't specify daemon host or port several times";
    return false;
  }

  if (m_generate_new.empty() && m_wallet_file_arg.empty()) {
    std::cout << "Nor 'generate-new-wallet' neither 'wallet-file' argument was specified.\nWhat do you want to do?\n[O]pen existing wallet, [G]enerate new wallet file or [E]xit.\n";
    char c;
    do {
      std::string answer;
      std::getline(std::cin, answer);
      c = answer[0];
      if (!(c == 'O' || c == 'G' || c == 'E' || c == 'o' || c == 'g' || c == 'e')) {
        std::cout << "Unknown command: " << c <<std::endl;
      } else {
        break;
      }
    } while (true);

    if (c == 'E' || c == 'e') {
      return false;
    }

    std::cout << "Specify wallet file name (e.g., wallet.bin).\n";
    std::string userInput;
    do {
      std::cout << "Wallet file name: ";
      std::getline(std::cin, userInput);
      boost::algorithm::trim(userInput);
    } while (userInput.empty());

    if (c == 'g' || c == 'G') {
      m_generate_new = userInput;
    } else {
      m_wallet_file_arg = userInput;
    }
	m_wallet_file_gui = userInput;
  }

  std::string walletFileName;
  if (!m_generate_new.empty()) {
    std::string ignoredString;
    WalletHelper::prepareFileNames(m_generate_new, ignoredString, walletFileName);
    boost::system::error_code ignore;
    if (boost::filesystem::exists(walletFileName, ignore)) {
      fail_msg_writer() << walletFileName << " already exists";
      return false;
    }
  }

  if (m_daemon_host.empty())
    m_daemon_host = "localhost";
  if (!m_daemon_port)
    m_daemon_port = RPC_DEFAULT_PORT;
  
  if (!m_daemon_address.empty()) {
    if (!parseUrlAddress(m_daemon_address, m_daemon_host, m_daemon_port)) {
      fail_msg_writer() << "failed to parse daemon address: " << m_daemon_address;
      return false;
    }
  } else {
    m_daemon_address = std::string("http://") + m_daemon_host + ":" + std::to_string(m_daemon_port);
  }

  Tools::PasswordContainer pwd_container;
  if (command_line::has_arg(vm, arg_password)) {
    pwd_container.password(command_line::get_arg(vm, arg_password));
  } else if (!pwd_container.read_password()) {
    fail_msg_writer() << "failed to read wallet password";
    return false;
  }

  this->m_node.reset(new NodeRpcProxy(m_daemon_host, m_daemon_port, logger.getLogger()));

  std::promise<std::error_code> errorPromise;
  std::future<std::error_code> f_error = errorPromise.get_future();
  auto callback = [&errorPromise](std::error_code e) {errorPromise.set_value(e); };

  m_node->addObserver(static_cast<INodeRpcProxyObserver*>(this));
  m_node->init(callback);
  auto error = f_error.get();
  if (error) {
    fail_msg_writer() << "failed to init NodeRPCProxy: " << error.message();
    return false;
  }

  m_pwd_arg = pwd_container.password();
  if (!m_generate_new.empty()) {
    std::string walletAddressFile = prepareWalletAddressFilename(m_generate_new);
    boost::system::error_code ignore;
    if (boost::filesystem::exists(walletAddressFile, ignore)) {
      logger(ERROR, BRIGHT_RED) << "Address file already exists: " + walletAddressFile;
      return false;
    }

    if (!new_wallet(walletFileName, pwd_container.password())) {
      logger(ERROR, BRIGHT_RED) << "account creation failed";
      return false;
    }

    if (!writeAddressFile(walletAddressFile, m_wallet->getAddress())) {
      logger(WARNING, BRIGHT_RED) << "Couldn't write wallet address file: " + walletAddressFile;
    }
  } else {
    m_wallet.reset(new WalletLegacy(m_currency, *m_node));

    try {
      m_wallet_file = tryToOpenWalletOrLoadKeysOrThrow(logger, m_wallet, m_wallet_file_arg, pwd_container.password());
	  m_wallet_file_gui = m_wallet_file_arg;
    } catch (const std::exception& e) {
      fail_msg_writer() << "failed to load wallet: " << e.what();
      return false;
    }

    m_wallet->addObserver(this);
    m_node->addObserver(static_cast<INodeObserver*>(this));

    logger(INFO, BRIGHT_WHITE) << "Opened wallet: " << m_wallet->getAddress();
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool mini_wallet::deinit() {
  m_wallet->removeObserver(this);
  m_node->removeObserver(static_cast<INodeObserver*>(this));
  m_node->removeObserver(static_cast<INodeRpcProxyObserver*>(this));

  if (!m_wallet.get())
    return true;

  return close_wallet();
}
//----------------------------------------------------------------------------------------------------
bool mini_wallet::new_wallet(const std::string &wallet_file, const std::string& password) {
  m_wallet_file = wallet_file;

  m_wallet.reset(new WalletLegacy(m_currency, *m_node.get()));
  m_node->addObserver(static_cast<INodeObserver*>(this));
  m_wallet->addObserver(this);
  try {
    m_initResultPromise.reset(new std::promise<std::error_code>());
    std::future<std::error_code> f_initError = m_initResultPromise->get_future();
    m_wallet->initAndGenerate(password);
    auto initError = f_initError.get();
    m_initResultPromise.reset(nullptr);
    if (initError) {
      fail_msg_writer() << "failed to generate new wallet: " << initError.message();
      return false;
    }

    try {
      CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
    } catch (std::exception& e) {
      fail_msg_writer() << "failed to save new wallet: " << e.what();
      throw;
    }

    AccountKeys keys;
    m_wallet->getAccountKeys(keys);

    logger(INFO, BRIGHT_WHITE) <<
      "Generated new wallet: " << m_wallet->getAddress() << std::endl <<
      "view key: " << Common::podToHex(keys.viewSecretKey);
  }
  catch (const std::exception& e) {
    fail_msg_writer() << "failed to generate new wallet: " << e.what();
    return false;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool mini_wallet::close_wallet()
{
  try {
    CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
  } catch (const std::exception& e) {
    fail_msg_writer() << e.what();
    return false;
  }

  m_wallet->removeObserver(this);
  m_wallet->shutdown();

  return true;
}

bool mini_wallet::reset(const std::vector<std::string> &args) {
	m_walletSynchronized = false;

	try {
		std::error_code saveError;
		std::stringstream ss;
		{
			SaveWaiter saveWaiter;
			WalletHelper::IWalletRemoveObserverGuard saveGuarantee(*m_wallet, saveWaiter);
			m_wallet->save(ss, false, false);
			saveError = saveWaiter.waitSave();
		}

		if (!saveError) {
			m_wallet->shutdown();
			InitWaiter initWaiter;
			WalletHelper::IWalletRemoveObserverGuard initGuarantee(*m_wallet, initWaiter);
			m_wallet->initAndLoad(ss, m_pwd_arg);
			initWaiter.waitInit();
		}
	}
	catch (std::exception& e) {
		std::cout << "exception in reset: " << e.what() << std::endl;
		return false;
	}

	return true;
}
//----------------------------------------------------------------------------------------------------
void mini_wallet::initCompleted(std::error_code result) {
  if (m_initResultPromise.get() != nullptr) {
    m_initResultPromise->set_value(result);
  }
}
//----------------------------------------------------------------------------------------------------
void mini_wallet::externalTransactionCreated(CryptoNote::TransactionId transactionId)  {
  WalletLegacyTransaction txInfo;
  m_wallet->getTransaction(transactionId, txInfo);
  
  std::stringstream logPrefix;
  if (txInfo.blockHeight == WALLET_LEGACY_UNCONFIRMED_TRANSACTION_HEIGHT) {
    logPrefix << "Unconfirmed";
  } else {
    logPrefix << "Height " << txInfo.blockHeight << ',';
  }

  if (txInfo.totalAmount >= 0) {
    logger(INFO, GREEN) <<
      logPrefix.str() << " transaction " << Common::podToHex(txInfo.hash) <<
      ", received " << m_currency.formatAmount(txInfo.totalAmount);
  } else {
    logger(INFO, MAGENTA) <<
      logPrefix.str() << " transaction " << Common::podToHex(txInfo.hash) <<
      ", spent " << m_currency.formatAmount(static_cast<uint64_t>(-txInfo.totalAmount));
  }

  if (txInfo.blockHeight == WALLET_LEGACY_UNCONFIRMED_TRANSACTION_HEIGHT) {
    m_refresh_progress_reporter.update(m_node->getLastLocalBlockHeight(), true);
  } else {
    m_refresh_progress_reporter.update(txInfo.blockHeight, true);
  }
}
//----------------------------------------------------------------------------------------------------
void mini_wallet::synchronizationCompleted(std::error_code result) {
  m_walletSynchronized = true;
  m_walletSynchronizedCV.notify_one();
}

void mini_wallet::synchronizationProgressUpdated(uint32_t current, uint32_t total) {
  if (!m_walletSynchronized) {
    m_refresh_progress_reporter.update(current, false);
  }
}
//----------------------------------------------------------------------------------------------------
bool mini_wallet::show_incoming_transfers(const std::vector<std::string>& args) {
  bool hasTransfers = false;
  size_t transactionsCount = m_wallet->getTransactionCount();
  for (size_t trantransactionNumber = 0; trantransactionNumber < transactionsCount; ++trantransactionNumber) {
    WalletLegacyTransaction txInfo;
    m_wallet->getTransaction(trantransactionNumber, txInfo);
    if (txInfo.totalAmount < 0) continue;
    hasTransfers = true;
    logger(INFO) << "        amount       \t                              tx id";
    logger(INFO, GREEN) <<  // spent - magenta
      std::setw(21) << m_currency.formatAmount(txInfo.totalAmount) << '\t' << Common::podToHex(txInfo.hash);
  }

  if (!hasTransfers) success_msg_writer() << "No incoming transfers";
  return true;
}
//----------------------------------------------------------------------------------------------------
std::string mini_wallet::transferGui(const std::vector<std::string> &args) {
	try {
		TransferCommand cmd(m_currency);

		if (!cmd.parseArguments(logger, args))
			return "Parse error";
		CryptoNote::WalletHelper::SendCompleteResultObserver sent;

		std::string extraString;
		std::copy(cmd.extra.begin(), cmd.extra.end(), std::back_inserter(extraString));

		WalletHelper::IWalletRemoveObserverGuard removeGuard(*m_wallet, sent);

		CryptoNote::TransactionId tx = m_wallet->sendTransaction(cmd.dsts, cmd.fee, extraString, cmd.fake_outs_count, 0);
		if (tx == WALLET_LEGACY_INVALID_TRANSACTION_ID) {
			return "Can't send money";
		}

		std::error_code sendError = sent.wait(tx);
		removeGuard.removeObserver();

		if (sendError) {
			return sendError.message();
		}

		CryptoNote::WalletLegacyTransaction txInfo;
		m_wallet->getTransaction(tx, txInfo);
		
		try {
			CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
		}
		catch (const std::exception& e) {
			return e.what();
		}
		return Common::podToHex(txInfo.hash);
	}
	catch (const std::system_error& e) {
		return e.what();
	}
	catch (const std::exception& e) {
		return e.what();
	}
	catch (...) {
		return "unknown error";
	}
}
//----------------------------------------------------------------------------------------------------
bool mini_wallet::run() {
  m_consoleHandler.start(false, "[wallet]: ", Common::Console::Color::BrightYellow);
  return true;
}
//----------------------------------------------------------------------------------------------------
void mini_wallet::stop() {
  m_consoleHandler.requestStop();
}
std::string mini_wallet::getWalletFile() {
	return m_wallet_file_gui;
}

std::string mini_wallet::getWalletAddress() {
	return m_wallet->getAddress();
}

std::string mini_wallet::getBalance() {
	std::string balanceString;
	if (!m_walletSynchronized) return balanceString;
	try {
		std::string balance = m_currency.formatAmount(m_wallet->actualBalance());
		std::string locked = m_currency.formatAmount(m_wallet->pendingBalance());
		balanceString = balance + "|" + locked;
	} catch (...) {
		balanceString = "";
	}
	return balanceString;
}

size_t mini_wallet::getTxsCount() {
	return m_wallet->getTransactionCount();
}

std::string mini_wallet::getTxs() {
	std::string txs = "";

	size_t transactionsCount = m_wallet->getTransactionCount();
	for (size_t trantransactionNumber = 0; trantransactionNumber < transactionsCount; ++trantransactionNumber) {
		WalletLegacyTransaction txInfo;
		m_wallet->getTransaction(trantransactionNumber, txInfo);
		if (txInfo.state != WalletLegacyTransactionState::Active || txInfo.blockHeight == WALLET_LEGACY_UNCONFIRMED_TRANSACTION_HEIGHT) {
			continue;
		}

		std::vector<uint8_t> extraVec = Common::asBinaryArray(txInfo.extra);

		Crypto::Hash paymentId;
		std::string paymentIdStr = (getPaymentIdFromTxExtra(extraVec, paymentId) && paymentId != NULL_HASH ? Common::podToHex(paymentId) : "");

		char timeString[20];
		time_t timestamp = static_cast<time_t>(txInfo.timestamp);
		if (std::strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", std::gmtime(&timestamp)) == 0) {
			throw std::runtime_error("time buffer is too small");
		}

		txs += std::string(timeString)
			+ "|" + Common::podToHex(txInfo.hash)
			+ "|" + m_currency.formatAmount(txInfo.totalAmount)
			+ "|" + m_currency.formatAmount(txInfo.fee)
			+ "|" + std::string(std::to_string(txInfo.blockHeight))
			+ "|" + std::string(std::to_string(txInfo.unlockTime));

		if (!paymentIdStr.empty()) {
			txs += "|" + paymentIdStr;
		}
		txs += "\n";
	}
	return txs;
}

std::string mini_wallet::transferWrapper(const std::vector<std::string> &args) {
	return transferGui(args);
}

bool mini_wallet::resetWrapper() {
	std::vector<std::string> args;
	reset(args);
	return true;
}

bool mini_wallet::openOutputFileStream(const std::string& filename, std::ofstream& file) {
	file.open(filename, std::ios_base::binary | std::ios_base::out | std::ios::trunc);
	if (file.fail()) {
		return false;
	}
	return true;
}

std::error_code mini_wallet::walletSaveWrapper(CryptoNote::IWalletLegacy& wallet, std::ofstream& file, bool saveDetailes, bool saveCache) {
	CryptoNote::WalletHelper::SaveWalletResultObserver o;

	std::error_code e;
	try {
		std::future<std::error_code> f = o.saveResult.get_future();
		wallet.addObserver(&o);
		wallet.save(file, saveDetailes, saveCache);
		e = f.get();
	}
	catch (std::exception&) {
		wallet.removeObserver(&o);
		return make_error_code(std::errc::invalid_argument);
	}

	wallet.removeObserver(&o);
	return e;
}

bool mini_wallet::saveWrapper(std::string m_walletFilename) {
	if (!m_walletSynchronized) return false;
	const std::string walletFilename = m_walletFilename + ".wallet";
	
	try {
		boost::filesystem::path tempFile = boost::filesystem::unique_path(walletFilename + ".tmp.%%%%-%%%%");

		if (boost::filesystem::exists(walletFilename)) {
			boost::filesystem::rename(walletFilename, tempFile);
		}

		std::ofstream file;
		try {
			openOutputFileStream(walletFilename, file);
		}
		catch (std::exception&) {
			if (boost::filesystem::exists(tempFile)) {
				boost::filesystem::rename(tempFile, walletFilename);
			}
			throw;
		}

		std::error_code saveError = walletSaveWrapper(*m_wallet, file, true, true);
		if (saveError) {
			file.close();
			boost::filesystem::remove(walletFilename);
			boost::filesystem::rename(tempFile, walletFilename);
			throw std::system_error(saveError);
		}

		file.close();

		boost::system::error_code ignore;
		boost::filesystem::remove(tempFile, ignore);
	} catch (...) {
		return false;
	}
	return true;
}

void wait(int seconds) {
	boost::this_thread::sleep_for(boost::chrono::seconds{ seconds });
}

void reset_helper(std::string m_wallet_file_gui, mini_wallet &wallet) {
	const std::string file_name_reset = m_wallet_file_gui + ".reset";
	boost::system::error_code ignore;

	while (true) {
		try {
			if (boost::filesystem::exists(file_name_reset, ignore)) {
				boost::filesystem::rename(file_name_reset, file_name_reset + "_");
				wallet.resetWrapper();
				wait(60);
			} else {
				wait(5);
			}
		}
		catch (...) {
			wait(2);
		}
	}
}

void save_helper(std::string m_wallet_file_gui, mini_wallet &wallet) {
	const std::string file_name_save = m_wallet_file_gui + ".save";
	boost::system::error_code ignore;

	while (true) {
		try {
			if (boost::filesystem::exists(file_name_save, ignore)) {
				boost::filesystem::rename(file_name_save, file_name_save + "_");
				wallet.saveWrapper(m_wallet_file_gui);
				wait(10);
			} else {
				wait(5);
			}
		} catch (...) {
			wait(2);
		}
	}
}

void tx_helper(std::string m_wallet_file_gui, mini_wallet &wallet) {
	boost::this_thread::interruption_enabled();
	const std::string file_name_txcast = m_wallet_file_gui + ".txcast";
	const std::string file_name_txresult = m_wallet_file_gui + ".txresult";
	std::ifstream file_stream_txcast;
	std::ofstream file_stream_txresult;
	boost::system::error_code ignore;

	while (true) {
		try {
			if (boost::filesystem::exists(file_name_txcast, ignore)) {
				if (!file_stream_txcast.is_open()) file_stream_txcast.open(file_name_txcast);
				std::string content;
				while (!file_stream_txcast.eof()) {
					file_stream_txcast >> content;
				}

				std::string delimiter = "|";
				size_t pos = 0;
				std::string token;
				uint8_t idx = 0;
				std::string mixin = "";
				std::string address = "";
				std::string amt = "";
				std::string paymentId = "";
				std::string fee = "";
				std::vector<std::string> args;

				while ((pos = content.find(delimiter)) != std::string::npos) {
					token = content.substr(0, pos);

					switch (idx) {
					case 0: { // mixin
						mixin = token;
						args.push_back(mixin);
						break;
					}
					case 1: { // address
						address = token;
						args.push_back(address);
						break;
					}
					case 2: { // amount
						amt = token;
						args.push_back(amt);
						break;
					}
					case 3: { // paymentId
						paymentId = token;
						if (paymentId != "") {
							args.push_back("-p");
							args.push_back(paymentId);
						}
						break;
					}
					case 4: { // fee
						fee = token;
						if (fee != "") {
							args.push_back("-f");
							args.push_back(fee);
						}
						break;
					}
					default: {
						break;
					}
					}

					idx++;
					content.erase(0, pos + delimiter.length());
				}
				// close file
				file_stream_txcast.close();
				// delete request
				uint8_t retry = 3;
				bool statusRemove = boost::filesystem::remove(file_name_txcast);
				while (retry > 0 && !statusRemove) {
					retry--;
					statusRemove = boost::filesystem::remove(file_name_txcast);
					wait(1);
				}
				// send tx
				std::string result = wallet.transferWrapper(args);
				// write result
				if (!file_stream_txresult.is_open()) file_stream_txresult.open(file_name_txresult);
				file_stream_txresult << result;
				file_stream_txresult.close();
			}
		} catch (...) {
			wait(1);
			continue;
		}
		
		boost::this_thread::interruption_point();
		wait(2);
	}
}

void gui_helper(std::string m_wallet_file_gui, mini_wallet &wallet) {
	boost::this_thread::interruption_enabled();
	m_wallet_file_gui = wallet.getWalletFile();
	const std::string file_name_status = m_wallet_file_gui + ".status";
	const std::string file_name_txs = m_wallet_file_gui + ".txs";
	const std::string file_name_address = m_wallet_file_gui + ".address";
	std::ofstream file_stream_status;
	std::ofstream file_stream_txs;
	std::ofstream file_stream_address;
	size_t lastTxsCount = 0;
	std::string lastTxs = "";
	std::string lastBalance = "";
	boost::system::error_code ignore;

	if (!boost::filesystem::exists(file_name_address, ignore)) {
		writeAddressFile(file_name_address, wallet.getWalletAddress());
	}

	while (true) {
		try {
			// get data
			std::string balance = wallet.getBalance();
			if (balance != "" && balance != lastBalance) {
				lastBalance = balance;

				// write on files
				if (!file_stream_status.is_open()) file_stream_status.open(file_name_status);
				file_stream_status << balance;
				file_stream_status.close();
			}

			std::string txs = wallet.getTxs();
			if (boost::filesystem::exists(file_name_txs, ignore) && txs == lastTxs) continue;
			lastTxs = txs;

			if (!file_stream_txs.is_open()) file_stream_txs.open(file_name_txs);
			file_stream_txs << txs;
			file_stream_txs.close();
		} catch (...) {
			wait(2);
			continue;
		}

		boost::this_thread::interruption_point();
		wait(5);
	}
}

int main(int argc, char* argv[]) {
#ifdef WIN32
  _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

  po::options_description desc_params("Wallet options");
  command_line::add_arg(desc_params, arg_wallet_file);
  command_line::add_arg(desc_params, arg_generate_new_wallet);
  command_line::add_arg(desc_params, arg_password);
  command_line::add_arg(desc_params, arg_daemon_address);
  command_line::add_arg(desc_params, arg_daemon_host);
  command_line::add_arg(desc_params, arg_daemon_port);
  Tools::wallet_rpc_server::init_options(desc_params);

  po::positional_options_description positional_options;

  po::options_description desc_all;
  
  Logging::LoggerManager logManager;
  Logging::LoggerRef logger(logManager, "miniwallet");
  System::Dispatcher dispatcher;

  po::variables_map vm;

  bool r = command_line::handle_error_helper(desc_all, [&]() {
    auto parser = po::command_line_parser(argc, argv).options(desc_params).positional(positional_options);
    po::store(parser.run(), vm);
    po::notify(vm);
    return true;
  });

  if (!r)
    return 1;

  //set up logging options
  Level logLevel = static_cast<Level>(2);

  logManager.configure(buildLoggerConfiguration(logLevel, Common::ReplaceExtenstion(argv[0], ".log")));

  logger(INFO, BRIGHT_WHITE) << CRYPTONOTE_NAME << " wallet v" << PROJECT_VERSION_LONG;

  CryptoNote::Currency currency = CryptoNote::CurrencyBuilder(logManager).
    testnet(false).currency();
	
	//runs wallet with console interface
	CryptoNote::mini_wallet wal(dispatcher, currency, logManager);
    
	if (!wal.init(vm)) {
		logger(ERROR, BRIGHT_RED) << "Failed to initialize wallet"; 
		return 1;
	}

	//bool gui_import = command_line::get_arg(vm, arg_gui_import);
	boost::thread t;
	boost::thread tx;
	boost::thread res;
	boost::thread save;
	std::string wallet_file = wal.getWalletFile();
	try {
		// Start threads
		boost::thread t(boost::bind(&gui_helper, wallet_file, boost::ref(wal)));
		t.detach();

		boost::thread tx(boost::bind(&tx_helper, wallet_file, boost::ref(wal)));
		tx.detach();

		boost::thread res(boost::bind(&reset_helper, wallet_file, boost::ref(wal)));
		res.detach();

		boost::thread save(boost::bind(&save_helper, wallet_file, boost::ref(wal)));
		save.detach();

		logger(INFO) << "GUI helper started: " << wallet_file;

		//if (gui_import) wal.resetWrapper();
	}
	catch (const std::exception& e) {
		logger(ERROR, BRIGHT_RED) << "failed to start GUI helper: " << e.what();
		return 1;
	}

	Tools::SignalHandler::install([&wal] {
		wal.stop();
	});
    
	wal.run();

	if (!wal.deinit()) {
		logger(ERROR, BRIGHT_RED) << "Failed to close wallet";
	} else {
		try {
			// Stop thread
			t.interrupt();
			tx.interrupt();
			res.interrupt();
			save.interrupt();
			logger(INFO) << "GUI helper stopped.";
		}
		catch (const std::exception& e) {
			logger(ERROR) << e.what();
			return 1;
		}
	}

  return 1;
  //CATCH_ENTRY_L0("main", 1);
}
