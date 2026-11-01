#include "PPC64RTAS.h"

#include <algorithm>
#include <stdexcept>

namespace ia64::ppc64 {
namespace {

PPC64RTASServiceResult parameterError() {
    PPC64RTASServiceResult result;
    result.status = PPC64_RTAS_OUT_PARAM_ERROR;
    return result;
}

PPC64RTASServiceDescriptor makeService(
    const char* name, uint32_t token, uint32_t nargs, uint32_t nret,
    PPC64RTASValidationPolicy policy,
    std::function<PPC64RTASServiceResult(
        PPC64XICS&, uint16_t, const std::vector<uint32_t>&)> handler) {
    PPC64RTASServiceDescriptor descriptor;
    descriptor.name = name;
    descriptor.token = token;
    descriptor.requiredArguments = nargs;
    descriptor.requiredReturns = nret;
    descriptor.requiredCapabilities = PPC64_RTAS_CAP_XICS;
    descriptor.validationPolicy = policy;
    descriptor.guestVisible = true;
    descriptor.handler = std::move(handler);
    return descriptor;
}

} // namespace

PPC64RTASServiceRegistry::PPC64RTASServiceRegistry() {
    std::string ignored;
    const auto policy = PPC64RTASValidationPolicy::ACTIVE_LPID_SOURCE_OWNERSHIP;
    const bool registered =
        registerService(makeService("ibm,set-xive", PPC64_RTAS_TOKEN_IBM_SET_XIVE,
            3, 1, policy,
            [](PPC64XICS& xics, uint16_t lpid,
               const std::vector<uint32_t>& args) {
                PPC64RTASServiceResult result;
                std::string reason;
                if (args[1] != PPC64_XICS_SERVER_ID || args[2] > 0xffU ||
                    !xics.setSourceXive(args[0], lpid,
                        static_cast<uint32_t>(args[1]),
                        static_cast<uint8_t>(args[2]), reason)) {
                    return parameterError();
                }
                return result;
            }), ignored) &&
        registerService(makeService("ibm,get-xive", PPC64_RTAS_TOKEN_IBM_GET_XIVE,
            1, 3, policy,
            [](PPC64XICS& xics, uint16_t lpid,
               const std::vector<uint32_t>& args) {
                PPC64RTASServiceResult result;
                PPC64XICSSourceSnapshot source;
                if (xics.activeLPID() != lpid ||
                    !xics.getSource(args[0], source) ||
                    source.ownerLPID != lpid) {
                    return parameterError();
                }
                result.values = {source.serverId, source.priority};
                return result;
            }), ignored) &&
        registerService(makeService("ibm,int-off", PPC64_RTAS_TOKEN_IBM_INT_OFF,
            1, 1, policy,
            [](PPC64XICS& xics, uint16_t lpid,
               const std::vector<uint32_t>& args) {
                PPC64RTASServiceResult result;
                std::string reason;
                if (!xics.rtasInterruptOff(args[0], lpid, reason)) {
                    return parameterError();
                }
                return result;
            }), ignored) &&
        registerService(makeService("ibm,int-on", PPC64_RTAS_TOKEN_IBM_INT_ON,
            1, 1, policy,
            [](PPC64XICS& xics, uint16_t lpid,
               const std::vector<uint32_t>& args) {
                PPC64RTASServiceResult result;
                std::string reason;
                if (!xics.rtasInterruptOn(args[0], lpid, reason)) {
                    return parameterError();
                }
                return result;
            }), ignored);
    if (!registered) {
        throw std::logic_error("failed to register the built-in PPC64 RTAS profile");
    }
}

bool PPC64RTASServiceRegistry::registerService(
    PPC64RTASServiceDescriptor descriptor, std::string& reason) {
    const bool validPolicy =
        descriptor.validationPolicy == PPC64RTASValidationPolicy::EXACT_CELL_COUNTS ||
        descriptor.validationPolicy ==
            PPC64RTASValidationPolicy::ACTIVE_LPID_SOURCE_OWNERSHIP;
    if (descriptor.name.empty() || descriptor.token == 0 ||
        descriptor.requiredArguments > PPC64_RTAS_MAX_CELLS ||
        descriptor.requiredReturns == 0 ||
        descriptor.requiredReturns > PPC64_RTAS_MAX_CELLS ||
        descriptor.requiredArguments > PPC64_RTAS_MAX_CELLS -
                                           descriptor.requiredReturns ||
        !validPolicy || !descriptor.handler) {
        reason = "RTAS service descriptor is invalid or exceeds the bounded cell profile";
        return false;
    }
    if (services_.find(descriptor.token) != services_.end()) {
        reason = "RTAS token is already registered";
        return false;
    }
    services_.emplace(descriptor.token, std::move(descriptor));
    reason.clear();
    return true;
}

const PPC64RTASServiceDescriptor* PPC64RTASServiceRegistry::find(
    uint32_t token) const {
    const auto found = services_.find(token);
    return found == services_.end() ? nullptr : &found->second;
}

PPC64RTASInvocationResult PPC64RTASServiceRegistry::invoke(
    uint32_t token, uint32_t nargs, uint32_t nret,
    const std::vector<uint32_t>& arguments, uint32_t availableCapabilities,
    PPC64XICS& xics, uint16_t activeLPID) const {
    PPC64RTASInvocationResult invocation;
    const PPC64RTASServiceDescriptor* descriptor = find(token);
    if (descriptor == nullptr || !descriptor->guestVisible) {
        if (nret != 0) {
            invocation.returnCells.push_back({0,
                static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR)});
        }
        return invocation;
    }
    invocation.recognized = true;

    PPC64RTASServiceResult result;
    if (arguments.size() != nargs ||
        (descriptor->requiredCapabilities & availableCapabilities) !=
            descriptor->requiredCapabilities ||
        nargs != descriptor->requiredArguments ||
        nret != descriptor->requiredReturns ||
        (descriptor->validationPolicy ==
             PPC64RTASValidationPolicy::ACTIVE_LPID_SOURCE_OWNERSHIP &&
         xics.activeLPID() != activeLPID)) {
        result = parameterError();
    } else {
        result = descriptor->handler(xics, activeLPID, arguments);
    }

    invocation.returnCells.push_back({0,
        static_cast<uint32_t>(result.status)});
    const uint32_t writableValues = std::min<uint32_t>(
        static_cast<uint32_t>(result.values.size()), nret > 0 ? nret - 1U : 0U);
    for (uint32_t index = 0; index < writableValues; ++index) {
        invocation.returnCells.push_back({index + 1U, result.values[index]});
    }
    return invocation;
}

} // namespace ia64::ppc64
