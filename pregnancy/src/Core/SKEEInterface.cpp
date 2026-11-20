#include "pch.h"
#include "Core/SKEEInterface.h"

namespace PNS {

    bool SKEEInterface::TryDispatch(const char* pluginName) {
        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) return false;

        InterfaceExchangeMessage exchangeMsg;
        bool dispatched = messaging->Dispatch(
            InterfaceExchangeMessage::kExchangeInterface,
            (void*)&exchangeMsg,
            sizeof(InterfaceExchangeMessage*),
            pluginName
        );

        logger::info("SKEEInterface: Dispatch to '{}' returned={}, interfaceMap={:p}",
                     pluginName, dispatched, (void*)exchangeMsg.interfaceMap);

        if (!dispatched || !exchangeMsg.interfaceMap) {
            return false;
        }

        ExtractInterfaces(exchangeMsg.interfaceMap);
        return IsAvailable();
    }

    void SKEEInterface::ExtractInterfaces(IInterfaceMap* map) {
        if (auto* bodyMorph = map->QueryInterface("BodyMorph")) {
            bodyMorphIntfc_ = static_cast<IBodyMorphInterface*>(bodyMorph);
            logger::info("SKEEInterface: Acquired IBodyMorphInterface v{}",
                         bodyMorphIntfc_->GetVersion());
        } else {
            logger::warn("SKEEInterface: BodyMorph interface not found");
        }

        if (auto* override_ = map->QueryInterface("Override")) {
            overrideIntfc_ = static_cast<IOverrideInterface*>(override_);
            logger::info("SKEEInterface: Acquired IOverrideInterface v{}",
                         overrideIntfc_->GetVersion());
        } else {
            logger::warn("SKEEInterface: Override interface not found");
        }

        if (IsAvailable()) {
            logger::info("SKEEInterface: All interfaces acquired successfully");
        } else {
            logger::warn("SKEEInterface: Some interfaces not available (BodyMorph={}, Override={})",
                         bodyMorphIntfc_ != nullptr, overrideIntfc_ != nullptr);
        }
    }

    void SKEEInterface::HandleSKEEMessage(SKSE::MessagingInterface::Message* msg) {
        auto& self = GetSingleton();
        if (self.IsAvailable()) return;  // Already have interfaces

        logger::info("SKEEInterface: Received message from SKEE — type=0x{:X}, dataLen={}",
                     msg->type, msg->dataLen);

        if (msg->type == InterfaceExchangeMessage::kExchangeInterface) {
            auto* exchangeMsg = static_cast<InterfaceExchangeMessage*>(msg->data);
            if (exchangeMsg && exchangeMsg->interfaceMap) {
                logger::info("SKEEInterface: Got interfaceMap via listener ({:p})",
                             (void*)exchangeMsg->interfaceMap);
                self.ExtractInterfaces(exchangeMsg->interfaceMap);
            } else {
                logger::warn("SKEEInterface: Listener received exchange message but interfaceMap is null");
            }
        }
    }

    void SKEEInterface::AcquireInterfaces() {
        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            logger::error("SKEEInterface: Failed to get SKSE messaging interface");
            return;
        }

        // Try dispatching to SKEE with known plugin names
        static const char* pluginNames[] = { "skee", "skee64", "skeevr" };
        for (auto* name : pluginNames) {
            if (TryDispatch(name)) {
                logger::info("SKEEInterface: Successfully acquired interfaces via '{}'", name);
                return;
            }
        }

        logger::warn("SKEEInterface: Dispatch approach failed for all names. "
                     "Registering listeners as fallback...");

        // Fallback: register to RECEIVE InterfaceExchangeMessage from SKEE
        // SKEE may dispatch its interfaces after our dispatch attempt
        for (auto* name : pluginNames) {
            bool registered = messaging->RegisterListener(name, HandleSKEEMessage);
            logger::info("SKEEInterface: RegisterListener('{}') = {}", name, registered);
        }
    }

    void SKEEInterface::RetryIfNeeded() {
        if (IsAvailable()) return;

        logger::info("SKEEInterface: Retrying interface acquisition at kDataLoaded...");

        static const char* pluginNames[] = { "skee", "skee64", "skeevr" };
        for (auto* name : pluginNames) {
            if (TryDispatch(name)) {
                logger::info("SKEEInterface: Retry succeeded via '{}'", name);
                return;
            }
        }

        logger::error("SKEEInterface: All acquisition attempts failed. "
                      "RaceMenu may not be installed or is incompatible.");
    }

    float SKEEInterface::GetMorph(RE::Actor* actor, const char* morphName, const char* key) {
        if (!bodyMorphIntfc_ || !actor) return 0.0f;
        return bodyMorphIntfc_->GetMorph(actor, morphName, key);
    }

    std::vector<std::string> SKEEInterface::GetMorphKeys(RE::Actor* actor, const char* morphName) {
        if (!bodyMorphIntfc_ || !actor) return {};
        return bodyMorphIntfc_->GetMorphKeys(actor, morphName);
    }

    void SKEEInterface::AddNodeOverrideString(RE::Actor* actor, bool isFemale, const char* node,
                                               std::uint16_t key, std::uint8_t index,
                                               const char* texturePath) {
        if (!overrideIntfc_ || !actor) return;

        logger::info("SKEEInterface: AddNodeOverride BEFORE call for {:08X} node='{}' key={} idx={} tex='{}'",
                     actor->GetFormID(), node, key, index, texturePath);

        IOverrideInterface::StringSetVariant variant(texturePath);
        overrideIntfc_->AddNodeOverride(actor, isFemale, node, key, index, variant);

        logger::info("SKEEInterface: AddNodeOverride returned, calling SetNodeProperties(immediate=true)");

        // Push the stored override to the actual NiNode properties (immediate=true)
        overrideIntfc_->SetNodeProperties(actor, true);

        logger::info("SKEEInterface: SetNodeProperties returned");

        // Also try ApplyNodeOverrides directly on the actor's 3D
        if (auto* root = actor->Get3D()) {
            logger::info("SKEEInterface: Calling ApplyNodeOverrides on 3D root '{}'", root->name.c_str());
            overrideIntfc_->ApplyNodeOverrides(actor, root, true);
            logger::info("SKEEInterface: ApplyNodeOverrides returned");
        }
    }

    void SKEEInterface::RemoveNodeOverride(RE::Actor* actor, bool isFemale, const char* node,
                                            std::uint16_t key, std::uint8_t index) {
        if (!overrideIntfc_ || !actor) return;
        overrideIntfc_->RemoveNodeOverride(actor, isFemale, node, key, index);
        overrideIntfc_->SetNodeProperties(actor, true);
    }

}  // namespace PNS

