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
/** @file Executive.cpp
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 */

#include "Executive.h"

#include <boost/timer.hpp>
#ifndef Anomaly_BUILD
#include <json/json.h>
#endif
#include <libdevcore/CommonIO.h>
#include <libevm/VMFactory.h>
#include <libevm/VM.h>
#include <libethcore/CommonJS.h>
#include "Interface.h"
#include "State.h"
#include "ExtVM.h"
#include "BlockChain.h"
#include "Block.h"
using namespace std;
using namespace dev;
using namespace dev::eth;

const char* VMTraceChannel::name() { return "EVM"; }
const char* ExecutiveWarnChannel::name() { return WarnChannel::name(); }

#ifdef Anomaly_BUILD
StandardTrace::StandardTrace()
{}
#else
StandardTrace::StandardTrace():
        m_trace(Json::arrayValue)
{}
#endif

bool changesMemory(Instruction _inst)
{
	return
		_inst == Instruction::MSTORE ||
		_inst == Instruction::MSTORE8 ||
		_inst == Instruction::MLOAD ||
		_inst == Instruction::CREATE ||
		_inst == Instruction::CALL ||
		_inst == Instruction::CALLCODE ||
		_inst == Instruction::SHA3 ||
		_inst == Instruction::CALLDATACOPY ||
		_inst == Instruction::CODECOPY ||
		_inst == Instruction::EXTCODECOPY ||
		_inst == Instruction::DELEGATECALL;
}

bool changesStorage(Instruction _inst)
{
	return _inst == Instruction::SSTORE;
}

void StandardTrace::operator()(uint64_t _steps, uint64_t PC, Instruction inst, bigint newMemSize, bigint gasCost, bigint gas, VM* voidVM, ExtVMFace const* voidExt)
{
#ifdef Anomaly_BUILD
    return;
#else
	(void)_steps;

	ExtVM const& ext = dynamic_cast<ExtVM const&>(*voidExt);
	VM& vm = *voidVM;

	Json::Value r(Json::objectValue);

	Json::Value stack(Json::arrayValue);
	if (!m_options.disableStack)
	{
		for (auto const& i: vm.stack())
			stack.append("0x" + toHex(toCompactBigEndian(i, 1)));
		r["stack"] = stack;
	}

	bool newContext = false;
	Instruction lastInst = Instruction::STOP;

	if (m_lastInst.size() == ext.depth)
	{
		// starting a new context
		assert(m_lastInst.size() == ext.depth);
		m_lastInst.push_back(inst);
		newContext = true;
	}
	else if (m_lastInst.size() == ext.depth + 2)
	{
		m_lastInst.pop_back();
		lastInst = m_lastInst.back();
	}
	else if (m_lastInst.size() == ext.depth + 1)
	{
		// continuing in previous context
		lastInst = m_lastInst.back();
		m_lastInst.back() = inst;
	}
	else
	{
		cwarn << "GAA!!! Tracing VM and more than one new/deleted stack frame between steps!";
		cwarn << "Attmepting naive recovery...";
		m_lastInst.resize(ext.depth + 1);
	}

	Json::Value memJson(Json::arrayValue);
	if (!m_options.disableMemory && (changesMemory(lastInst) || newContext))
	{
		for (unsigned i = 0; i < vm.memory().size(); i += 32)
		{
			bytesConstRef memRef(vm.memory().data() + i, 32);
			memJson.append(toHex(memRef, 2, HexPrefix::DontAdd));
		}
		r["memory"] = memJson;
	}

	if (!m_options.disableStorage && (m_options.fullStorage || changesStorage(lastInst) || newContext))
	{
		Json::Value storage(Json::objectValue);
		for (auto const& i: ext.state().storage(ext.myAddress))
			storage["0x" + toHex(toCompactBigEndian(i.second.first, 1))] = "0x" + toHex(toCompactBigEndian(i.second.second, 1));
		r["storage"] = storage;
	}

	if (m_showMnemonics)
		r["op"] = instructionInfo(inst).name;
	r["pc"] = toString(PC);
	r["gas"] = toString(gas);
	r["gasCost"] = toString(gasCost);
	if (!!newMemSize)
		r["memexpand"] = toString(newMemSize);

	m_trace.append(r);
#endif
}

string StandardTrace::json(bool _styled) const
{
#ifdef Anomaly_BUILD
    return "";
#else
	return _styled ? Json::StyledWriter().write(m_trace) : Json::FastWriter().write(m_trace);
#endif
}

Executive::Executive(Block& _s, BlockChain const& _bc, unsigned _level):
	m_s(_s.mutableState()),
	m_envInfo(_s.info(), _bc.lastHashes(_s.info().parentHash())),
	m_depth(_level),
	m_sealEngine(*_bc.sealEngine())
{
}

Executive::Executive(Block& _s, LastHashes const& _lh, unsigned _level):
	m_s(_s.mutableState()),
	m_envInfo(_s.info(), _lh),
	m_depth(_level),
	m_sealEngine(*_s.sealEngine())
{
}

Executive::Executive(State& _s, Block const& _block, unsigned _txIndex, BlockChain const& _bc, unsigned _level):
	m_s(_s = _block.fromPending(_txIndex)),
	m_envInfo(_block.info(), _bc.lastHashes(_block.info().parentHash()), _txIndex ? _block.receipt(_txIndex - 1).gasUsed() : 0),
	m_depth(_level),
	m_sealEngine(*_bc.sealEngine())
{
}

u256 Executive::gasUsed() const
{
	return m_t.gas() - m_gas;
}

void Executive::accrueSubState(SubState& _parentContext)
{
	if (m_ext)
		_parentContext += m_ext->sub;
}

void Executive::initialize(Transaction const& _transaction)
{
	m_t = _transaction;

	// Avoid transactions that would take us beyond the block gas limit.
	u256 startGasUsed = m_envInfo.gasUsed();
	if (startGasUsed + (bigint)m_t.gas() > m_envInfo.gasLimit())
	{
		clog(ExecutiveWarnChannel) << "Cannot fit tx in block" << m_envInfo.number() << ": Require <" << (m_envInfo.gasLimit() - startGasUsed) << " Got" << m_t.gas();
		m_excepted = TransactionException::BlockGasLimitReached;
		BOOST_THROW_EXCEPTION(BlockGasLimitReached() << RequirementError((bigint)(m_envInfo.gasLimit() - startGasUsed), (bigint)m_t.gas()));
	}

	// Check gas cost is enough.
	m_baseGasRequired = m_t.baseGasRequired(m_sealEngine.evmSchedule(m_envInfo));
	if (m_baseGasRequired > m_t.gas())
	{
		clog(ExecutiveWarnChannel) << "Not enough gas to pay for the transaction: Require >" << m_baseGasRequired << " Got" << m_t.gas();
		m_excepted = TransactionException::OutOfGasBase;
		BOOST_THROW_EXCEPTION(OutOfGasBase() << RequirementError((bigint)m_baseGasRequired, (bigint)m_t.gas()));
	}

	// Avoid invalid transactions.
	u256 nonceReq;
	try
	{
		nonceReq = m_s.getNonce(m_t.sender());
	}
	catch (...)
	{
		clog(ExecutiveWarnChannel) << "Invalid Signature";
		m_excepted = TransactionException::InvalidSignature;
		throw;
	}
	if (m_t.nonce() != nonceReq)
	{
		clog(ExecutiveWarnChannel) << "Invalid Nonce: Require" << nonceReq << " Got" << m_t.nonce();
		m_excepted = TransactionException::InvalidNonce;
		BOOST_THROW_EXCEPTION(InvalidNonce() << RequirementError((bigint)nonceReq, (bigint)m_t.nonce()));
	}

	// Avoid unaffordable transactions.
	bigint gasCost = (bigint)m_t.gas() * m_t.gasPrice();
	bigint totalCost = m_t.value() + gasCost;
	if (m_s.balance(m_t.sender()) < totalCost)
	{
		clog(ExecutiveWarnChannel) << "Not enough cash: Require >" << totalCost << "=" << m_t.gas() << "*" << m_t.gasPrice() << "+" << m_t.value() << " Got" << m_s.balance(m_t.sender()) << "for sender: " << m_t.sender();
		m_excepted = TransactionException::NotEnoughCash;
		BOOST_THROW_EXCEPTION(NotEnoughCash() << RequirementError(totalCost, (bigint)m_s.balance(m_t.sender())) << errinfo_comment(m_t.sender().abridged()));
	}
	m_gasCost = (u256)gasCost;  // Convert back to 256-bit, safe now.
}

bool Executive::execute()
{
	// Entry point for a user-executed transaction.

	// Pay...
	clog(StateDetail) << "Paying" << formatBalance(m_gasCost) << "from sender for gas (" << m_t.gas() << "gas at" << formatBalance(m_t.gasPrice()) << ")";
	m_s.subBalance(m_t.sender(), m_gasCost);

	if (m_t.isCreation())
		return create(m_t.sender(), m_t.value(), m_t.gasPrice(), m_t.gas() - (u256)m_baseGasRequired, &m_t.data(), m_t.sender());
	else
		return call(m_t.receiveAddress(), m_t.sender(), m_t.value(), m_t.gasPrice(), bytesConstRef(&m_t.data()), m_t.gas() - (u256)m_baseGasRequired);
}

bool Executive::call(Address _receiveAddress, Address _senderAddress, u256 _value, u256 _gasPrice, bytesConstRef _data, u256 _gas)
{
	CallParameters params{_senderAddress, _receiveAddress, _receiveAddress, _value, _value, _gas, _data, {}};
	return call(params, _gasPrice, _senderAddress);
}

bool Executive::call(CallParameters const& _p, u256 const& _gasPrice, Address const& _origin)
{
	// If external transaction.
	if (m_t)
	{
		// FIXME: changelog contains unrevertable balance change that paid
		//        for the transaction.
		// Increment associated nonce for sender.
		m_s.incNonce(_p.senderAddress);
	}

	m_savepoint = m_s.savepoint();

	if (m_sealEngine.isPrecompiled(_p.codeAddress, m_envInfo.number()))
	{
		bigint g = m_sealEngine.costOfPrecompiled(_p.codeAddress, _p.data, m_envInfo.number());
		if (_p.gas < g)
		{
			m_excepted = TransactionException::OutOfGasBase;
			// Bail from exception.
			
			// Empty precompiled contracts need to be deleted even in case of OOG
			// because the bug in both Geth and Parity led to deleting RIPEMD precompiled in this case
			// see https://github.com/ethereum/go-ethereum/pull/3341/files#diff-2433aa143ee4772026454b8abd76b9dd
			// We mark the account as touched here, so that is can be removed among other touched empty accounts (after tx finalization)
			if (m_envInfo.number() >= m_sealEngine.chainParams().u256Param("EIP158ForkBlock"))
				m_s.addBalance(_p.codeAddress, 0);
			
			return true;	// true actually means "all finished - nothing more to be done regarding go().
		}
		else
		{
			m_gas = (u256)(_p.gas - g);
			bytes output;
			bool success;
			tie(success, output) = m_sealEngine.executePrecompiled(_p.codeAddress, _p.data, m_envInfo.number());
			if (!success)
			{
				m_gas = 0;
				m_excepted = TransactionException::OutOfGas;
			}
			size_t outputSize = output.size();
			m_output = owning_bytes_ref{std::move(output), 0, outputSize};
		}
	}
	else
	{
		m_gas = _p.gas;
		if (m_s.addressHasCode(_p.codeAddress))
		{
			bytes const& c = m_s.code(_p.codeAddress);
			h256 codeHash = m_s.codeHash(_p.codeAddress);
			m_ext = make_shared<ExtVM>(m_s, m_envInfo, m_sealEngine, _p.receiveAddress, _p.senderAddress, _origin, _p.apparentValue, _gasPrice, _p.data, &c, codeHash, m_depth);
		}
	}

	//////////////////////////////////////////////// // Anomaly
	if(!m_s.addressInUse(_p.receiveAddress))
		m_sealEngine.deleteAddresses.insert(_p.receiveAddress);
	////////////////////////////////////////////////

	// Transfer ether.
	m_s.transferBalance(_p.senderAddress, _p.receiveAddress, _p.valueTransfer);
	return !m_ext;
}

bool Executive::create(Address _sender, u256 _endowment, u256 _gasPrice, u256 _gas, bytesConstRef _init, Address _origin)
{
	u256 nonce = m_s.getNonce(_sender);
	m_s.incNonce(_sender);

	m_savepoint = m_s.savepoint();

	m_isCreation = true;

	// We can allow for the reverted state (i.e. that with which m_ext is constructed) to contain the m_orig.address, since
	// we delete it explicitly if we decide we need to revert.
	m_newAddress = right160(sha3(rlpList(_sender, nonce)));
	m_gas = _gas;

	// Transfer ether before deploying the code. This will also create new
	// account if it does not exist yet.
	m_s.transferBalance(_sender, m_newAddress, _endowment);

	if (m_envInfo.number() >= m_sealEngine.chainParams().u256Param("EIP158ForkBlock"))
		m_s.incNonce(m_newAddress);

	// Schedule _init execution if not empty.
	if (!_init.empty())
		m_ext = make_shared<ExtVM>(m_s, m_envInfo, m_sealEngine, m_newAddress, _sender, _origin, _endowment, _gasPrice, bytesConstRef(), _init, sha3(_init), m_depth);
	else if (m_s.addressHasCode(m_newAddress))
		// Overwrite with empty code in case the account already has a code
		// (address collision -- not real life case but we can check it with
		// synthetic tests).
		m_s.setNewCode(m_newAddress, {});

	return !m_ext;
}

OnOpFunc Executive::simpleTrace()
{
	return [](uint64_t steps, uint64_t PC, Instruction inst, bigint newMemSize, bigint gasCost, bigint gas, VM* voidVM, ExtVMFace const* voidExt)
	{
		ExtVM const& ext = *static_cast<ExtVM const*>(voidExt);
		VM& vm = *voidVM;

		ostringstream o;
		o << endl << "    STACK" << endl;
		for (auto i: vm.stack())
			o << (h256)i << endl;
		o << "    MEMORY" << endl << ((vm.memory().size() > 1000) ? " mem size greater than 1000 bytes " : memDump(vm.memory()));
		o << "    STORAGE" << endl;
		for (auto const& i: ext.state().storage(ext.myAddress))
			o << showbase << hex << i.second.first << ": " << i.second.second << endl;
		dev::LogOutputStream<VMTraceChannel, false>() << o.str();
		dev::LogOutputStream<VMTraceChannel, false>() << " < " << dec << ext.depth << " : " << ext.myAddress << " : #" << steps << " : " << hex << setw(4) << setfill('0') << PC << " : " << instructionInfo(inst).name << " : " << dec << gas << " : -" << dec << gasCost << " : " << newMemSize << "x32" << " >";
	};
}

bool Executive::go(OnOpFunc const& _onOp)
{
	if (m_ext)
	{
#if ETH_TIMED_EXECUTIONS
		Timer t;
#endif
		try
		{
			// Create VM instance. Force Interpreter if tracing requested.
			auto vm = _onOp ? VMFactory::create(VMKind::Interpreter) : VMFactory::create();
			if (m_isCreation)
			{
				auto out = vm->exec(m_gas, *m_ext, _onOp);
				if (m_res)
				{
					m_res->gasForDeposit = m_gas;
					m_res->depositSize = out.size();
				}
				if (out.size() > m_ext->evmSchedule().maxCodeSize)
					BOOST_THROW_EXCEPTION(OutOfGas());
				else if (out.size() * m_ext->evmSchedule().createDataGas <= m_gas)
				{
					if (m_res)
						m_res->codeDeposit = CodeDeposit::Success;
					m_gas -= out.size() * m_ext->evmSchedule().createDataGas;
				}
				else
				{
					if (m_ext->evmSchedule().exceptionalFailedCodeDeposit)
						BOOST_THROW_EXCEPTION(OutOfGas());
					else
					{
						if (m_res)
							m_res->codeDeposit = CodeDeposit::Failed;
						out = {};
					}
				}
				if (m_res)
					m_res->output = out.toVector(); // copy output to execution result
				m_s.setNewCode(m_ext->myAddress, out.toVector());
			}
			else
			{
				m_output = vm->exec(m_gas, *m_ext, _onOp);
				if (m_res)
					// Copy full output:
					m_res->output = m_output.toVector();
			}
		}
		catch (VMException const& _e)
		{
			clog(StateSafeExceptions) << "Safe VM Exception. " << diagnostic_information(_e);
			m_gas = 0;
			m_excepted = toTransactionException(_e);
			revert();
		}
		catch (Exception const& _e)
		{
			// TODO: AUDIT: check that this can never reasonably happen. Consider what to do if it does.
			cwarn << "Unexpected exception in VM. There may be a bug in this implementation. " << diagnostic_information(_e);
			exit(1);
			// Another solution would be to reject this transaction, but that also
			// has drawbacks. Essentially, the amount of ram has to be increased here.
		}
		catch (std::exception const& _e)
		{
			// TODO: AUDIT: check that this can never reasonably happen. Consider what to do if it does.
			cwarn << "Unexpected std::exception in VM. Not enough RAM? " << _e.what();
			exit(1);
			// Another solution would be to reject this transaction, but that also
			// has drawbacks. Essentially, the amount of ram has to be increased here.
		}
#if ETH_TIMED_EXECUTIONS
		cnote << "VM took:" << t.elapsed() << "; gas used: " << (sgas - m_endGas);
#endif
	}
	return true;
}

void Executive::finalize()
{
	// Accumulate refunds for suicides.
	if (m_ext)
		m_ext->sub.refunds += m_ext->evmSchedule().suicideRefundGas * m_ext->sub.suicides.size();

	// SSTORE refunds...
	// must be done before the miner gets the fees.
	m_refunded = m_ext ? min((m_t.gas() - m_gas) / 2, m_ext->sub.refunds) : 0;
	m_gas += m_refunded;

	if (m_t)
	{
		m_s.addBalance(m_t.sender(), m_gas * m_t.gasPrice());

		u256 feesEarned = (m_t.gas() - m_gas) * m_t.gasPrice();
		m_s.addBalance(m_envInfo.author(), feesEarned);
	}

	// Suicides...
	if (m_ext)
		for (auto a: m_ext->sub.suicides)
			m_s.kill(a);

	// Logs..
	if (m_ext)
		m_logs = m_ext->sub.logs;

	if (m_res) // Collect results
	{
		m_res->gasUsed = gasUsed();
		m_res->excepted = m_excepted; // TODO: m_except is used only in ExtVM::call
		m_res->newAddress = m_newAddress;
		m_res->gasRefunded = m_ext ? m_ext->sub.refunds : 0;
	}
}

void Executive::revert()
{
	if (m_ext)
		m_ext->sub.clear();

	// Set result address to the null one.
	m_newAddress = {};
	m_s.rollback(m_savepoint);
}
