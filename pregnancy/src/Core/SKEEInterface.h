#pragma once

namespace PNS {

    // SKEE (RaceMenu) interface types.
    // Protocol: consumer Dispatches InterfaceExchangeMessage to "skee" plugin,
    // SKEE fills in the interfaceMap pointer, consumer queries by string name.
    // Vtable layouts must match expired6978/SKSE64Plugins exactly.
    // Reference: expired6978/SKSE64Plugins/skee64/IPluginInterface.h

    // Base interface for all SKEE interfaces — has virtual destructor
    class IPluginInterface {
    public:
        IPluginInterface() {}
        virtual ~IPluginInterface() {}

        virtual std::uint32_t GetVersion() = 0;
        virtual void Revert() = 0;
    };

    // SKEE's interface map — NO virtual destructor (critical for vtable alignment)
    class IInterfaceMap {
    public:
        virtual IPluginInterface* QueryInterface(const char* name) = 0;
        virtual bool AddInterface(const char* name, IPluginInterface* iface) = 0;
        virtual IPluginInterface* RemoveInterface(const char* name) = 0;
    };

    // Message struct for interface exchange with SKEE
    struct InterfaceExchangeMessage {
        enum : std::uint32_t {
            kExchangeInterface = 0x9E3779B9
        };
        IInterfaceMap* interfaceMap = nullptr;
    };

    // Matches SKEE's IBodyMorphInterface vtable layout exactly.
    // Every virtual method must be declared in order even if unused.
    // Reference: expired6978/SKSE64Plugins/skee64/IPluginInterface.h
    class IBodyMorphInterface : public IPluginInterface {
    public:
        // Visitor types (opaque — we pass our own structs via void*)
        class MorphVisitor { public: virtual void Visit(RE::TESObjectREFR*, const char*) = 0; };
        class MorphKeyVisitor { public: virtual void Visit(const char*, float) = 0; };
        class MorphValueVisitor { public: virtual void Visit(RE::TESObjectREFR*, const char*, const char*, float) = 0; };
        class MorphActorVisitor { public: virtual void Visit(RE::TESObjectREFR*) = 0; };
        class StringVisitor { public: virtual void Visit(const char*) = 0; };
        class ActorVisitor { public: virtual void Visit(RE::TESObjectREFR*) = 0; };

        virtual void SetMorph(RE::TESObjectREFR* actor, const char* morphName, const char* key, float value) = 0;
        virtual float GetMorph(RE::TESObjectREFR* actor, const char* morphName, const char* key) = 0;
        virtual void ClearMorph(RE::TESObjectREFR* actor, const char* morphName, const char* key) = 0;
        virtual float GetBodyMorphs(RE::TESObjectREFR* actor, const char* morphName) = 0;
        virtual void ClearBodyMorphNames(RE::TESObjectREFR* actor, const char* morphName) = 0;
        virtual void VisitMorphs(RE::TESObjectREFR* actor, MorphVisitor& visitor) = 0;
        virtual void VisitKeys(RE::TESObjectREFR* actor, const char* name, MorphKeyVisitor& visitor) = 0;
        virtual void VisitMorphValues(RE::TESObjectREFR* actor, MorphValueVisitor& visitor) = 0;
        virtual void ClearMorphs(RE::TESObjectREFR* actor) = 0;
        virtual void ApplyVertexDiff(RE::TESObjectREFR* refr, RE::NiAVObject* rootNode, bool erase = false) = 0;
        virtual void ApplyBodyMorphs(RE::TESObjectREFR* refr, bool deferUpdate = true) = 0;
        virtual void UpdateModelWeight(RE::TESObjectREFR* refr, bool immediate = false) = 0;
        virtual void SetCacheLimit(std::size_t limit) = 0;
        virtual bool HasMorphs(RE::TESObjectREFR* actor) = 0;
        virtual std::uint32_t EvaluateBodyMorphs(RE::TESObjectREFR* actor) = 0;
        virtual bool HasBodyMorph(RE::TESObjectREFR* actor, const char* morphName, const char* morphKey) = 0;
        virtual bool HasBodyMorphName(RE::TESObjectREFR* actor, const char* morphName) = 0;
        virtual bool HasBodyMorphKey(RE::TESObjectREFR* actor, const char* morphKey) = 0;
        virtual void ClearBodyMorphKeys(RE::TESObjectREFR* actor, const char* morphKey) = 0;
        virtual void VisitStrings(StringVisitor& visitor) = 0;
        virtual void VisitActors(ActorVisitor& visitor) = 0;
        virtual std::size_t ClearMorphCache() = 0;

        // Helper: collect morph keys via VisitKeys
        std::vector<std::string> GetMorphKeys(RE::TESObjectREFR* actor, const char* morphName) {
            std::vector<std::string> keys;
            struct KeyCollector : MorphKeyVisitor {
                std::vector<std::string>* keys;
                KeyCollector(std::vector<std::string>* k) : keys(k) {}
                void Visit(const char* key, float) override { if (key) keys->push_back(key); }
            };
            KeyCollector collector(&keys);
            VisitKeys(actor, morphName, collector);
            return keys;
        }
    };

    // Matches SKEE's IOverrideInterface vtable layout exactly.
    // Every virtual method must be declared in order even if unused.
    // Reference: expired6978/SKSE64Plugins/skee64/IPluginInterface.h
    class IOverrideInterface : public IPluginInterface {
    public:
        // Variant types for get/set operations
        class GetVariant {
        public:
            virtual void Int(std::int32_t i) = 0;
            virtual void Float(float f) = 0;
            virtual void String(const char* str) = 0;
            virtual void Bool(bool b) = 0;
            virtual void TextureSet(RE::BGSTextureSet* textureSet) = 0;
        };

        class SetVariant {
        public:
            enum class Type { None, Int, Float, String, Bool, TextureSet };
            virtual Type GetType() { return Type::None; }
            virtual std::int32_t Int() { return 0; }
            virtual float Float() { return 0.0f; }
            virtual const char* String() { return nullptr; }
            virtual bool Bool() { return false; }
            virtual RE::BGSTextureSet* TextureSet() { return nullptr; }
        };

        // String variant for setting texture paths
        class StringSetVariant : public SetVariant {
            const char* str_;
        public:
            StringSetVariant(const char* s) : str_(s) {}
            Type GetType() override { return Type::String; }
            const char* String() override { return str_; }
        };

        // Armor override methods (vtable slots — must be in exact order)
        virtual bool HasArmorAddonNode(RE::TESObjectREFR* refr, bool firstPerson,
            RE::TESObjectARMO* armor, RE::TESObjectARMA* addon,
            const char* nodeName, bool debug) = 0;
        virtual bool HasArmorOverride(RE::TESObjectREFR* refr, bool isFemale,
            RE::TESObjectARMO* armor, RE::TESObjectARMA* addon,
            const char* nodeName, std::uint16_t key, std::uint8_t index) = 0;
        virtual void AddArmorOverride(RE::TESObjectREFR* refr, bool isFemale,
            RE::TESObjectARMO* armor, RE::TESObjectARMA* addon,
            const char* nodeName, std::uint16_t key, std::uint8_t index,
            SetVariant& value) = 0;
        virtual bool GetArmorOverride(RE::TESObjectREFR* refr, bool isFemale,
            RE::TESObjectARMO* armor, RE::TESObjectARMA* addon,
            const char* nodeName, std::uint16_t key, std::uint8_t index,
            GetVariant& visitor) = 0;
        virtual void RemoveArmorOverride(RE::TESObjectREFR* refr, bool isFemale,
            RE::TESObjectARMO* armor, RE::TESObjectARMA* addon,
            const char* nodeName, std::uint16_t key, std::uint8_t index) = 0;
        virtual void SetArmorProperties(RE::TESObjectREFR* refr, bool immediate) = 0;
        virtual void SetArmorProperty(RE::TESObjectREFR* refr, bool firstPerson,
            RE::TESObjectARMO* armor, RE::TESObjectARMA* addon,
            const char* nodeName, std::uint16_t key, std::uint8_t index,
            SetVariant& value, bool immediate) = 0;
        virtual bool GetArmorProperty(RE::TESObjectREFR* refr, bool firstPerson,
            RE::TESObjectARMO* armor, RE::TESObjectARMA* addon,
            const char* nodeName, std::uint16_t key, std::uint8_t index,
            GetVariant& value) = 0;
        virtual void ApplyArmorOverrides(RE::TESObjectREFR* refr,
            RE::TESObjectARMO* armor, RE::TESObjectARMA* addon,
            RE::NiAVObject* object, bool immediate) = 0;
        virtual void RemoveAllArmorOverrides() = 0;
        virtual void RemoveAllArmorOverridesByReference(RE::TESObjectREFR* reference) = 0;
        virtual void RemoveAllArmorOverridesByArmor(RE::TESObjectREFR* refr, bool isFemale,
            RE::TESObjectARMO* armor) = 0;
        virtual void RemoveAllArmorOverridesByAddon(RE::TESObjectREFR* refr, bool isFemale,
            RE::TESObjectARMO* armor, RE::TESObjectARMA* addon) = 0;
        virtual void RemoveAllArmorOverridesByNode(RE::TESObjectREFR* refr, bool isFemale,
            RE::TESObjectARMO* armor, RE::TESObjectARMA* addon,
            const char* nodeName) = 0;

        // Node override methods — these are what PNS uses
        virtual bool HasNodeOverride(RE::TESObjectREFR* refr, bool isFemale,
            const char* nodeName, std::uint16_t key, std::uint8_t index) = 0;
        virtual void AddNodeOverride(RE::TESObjectREFR* refr, bool isFemale,
            const char* nodeName, std::uint16_t key, std::uint8_t index,
            SetVariant& value) = 0;
        virtual bool GetNodeOverride(RE::TESObjectREFR* refr, bool isFemale,
            const char* nodeName, std::uint16_t key, std::uint8_t index,
            GetVariant& visitor) = 0;
        virtual void RemoveNodeOverride(RE::TESObjectREFR* refr, bool isFemale,
            const char* nodeName, std::uint16_t key, std::uint8_t index) = 0;
        virtual void SetNodeProperties(RE::TESObjectREFR* refr, bool immediate) = 0;
        virtual void SetNodeProperty(RE::TESObjectREFR* refr, bool firstPerson,
            const char* nodeName, std::uint16_t key, std::uint8_t index,
            SetVariant& value, bool immediate) = 0;
        virtual bool GetNodeProperty(RE::TESObjectREFR* refr, bool firstPerson,
            const char* nodeName, std::uint16_t key, std::uint8_t index,
            GetVariant& value) = 0;
        virtual void ApplyNodeOverrides(RE::TESObjectREFR* refr,
            RE::NiAVObject* object, bool immediate) = 0;
        virtual void RemoveAllNodeOverrides() = 0;
        virtual void RemoveAllNodeOverridesByReference(RE::TESObjectREFR* reference) = 0;
        virtual void RemoveAllNodeOverridesByNode(RE::TESObjectREFR* refr, bool isFemale,
            const char* nodeName) = 0;

        // Skin override methods (complete vtable — we don't use these but need the slots)
        virtual bool HasSkinOverride(RE::TESObjectREFR* refr, bool isFemale, bool firstPerson,
            std::uint32_t slotMask, std::uint16_t key, std::uint8_t index) = 0;
        virtual void AddSkinOverride(RE::TESObjectREFR* refr, bool isFemale, bool firstPerson,
            std::uint32_t slotMask, std::uint16_t key, std::uint8_t index,
            SetVariant& value) = 0;
        virtual bool GetSkinOverride(RE::TESObjectREFR* refr, bool isFemale, bool firstPerson,
            std::uint32_t slotMask, std::uint16_t key, std::uint8_t index,
            GetVariant& visitor) = 0;
        virtual void RemoveSkinOverride(RE::TESObjectREFR* refr, bool isFemale, bool firstPerson,
            std::uint32_t slotMask, std::uint16_t key, std::uint8_t index) = 0;
        virtual void SetSkinProperties(RE::TESObjectREFR* refr, bool immediate) = 0;
        virtual void SetSkinProperty(RE::TESObjectREFR* refr, bool firstPerson,
            std::uint32_t slotMask, std::uint16_t key, std::uint8_t index,
            SetVariant& value, bool immediate) = 0;
        virtual bool GetSkinProperty(RE::TESObjectREFR* refr, bool firstPerson,
            std::uint32_t slotMask, std::uint16_t key, std::uint8_t index,
            GetVariant& value) = 0;
        virtual void ApplySkinOverrides(RE::TESObjectREFR* refr, bool firstPerson,
            RE::TESObjectARMO* armor, RE::TESObjectARMA* addon,
            std::uint32_t slotMask, RE::NiAVObject* object, bool immediate) = 0;
        virtual void RemoveAllSkinOverrides() = 0;
        virtual void RemoveAllSkinOverridesByReference(RE::TESObjectREFR* reference) = 0;
        virtual void RemoveAllSkinOverridesBySlot(RE::TESObjectREFR* refr, bool isFemale,
            bool firstPerson, std::uint32_t slotMask) = 0;
    };

    // Singleton manager for acquired SKEE interfaces
    class SKEEInterface {
    public:
        static SKEEInterface& GetSingleton() {
            static SKEEInterface instance;
            return instance;
        }

        // Call during kPostPostLoad to exchange interfaces with SKEE
        void AcquireInterfaces();

        // Retry acquisition (call at kDataLoaded if kPostPostLoad failed)
        void RetryIfNeeded();

        bool IsAvailable() const { return bodyMorphIntfc_ != nullptr && overrideIntfc_ != nullptr; }

        IBodyMorphInterface* GetBodyMorphInterface() const { return bodyMorphIntfc_; }
        IOverrideInterface* GetOverrideInterface() const { return overrideIntfc_; }

        // Convenience wrappers
        float GetMorph(RE::Actor* actor, const char* morphName, const char* key);
        std::vector<std::string> GetMorphKeys(RE::Actor* actor, const char* morphName);
        void AddNodeOverrideString(RE::Actor* actor, bool isFemale, const char* node,
                                   std::uint16_t key, std::uint8_t index,
                                   const char* texturePath);
        void RemoveNodeOverride(RE::Actor* actor, bool isFemale, const char* node,
                                std::uint16_t key, std::uint8_t index);

        // Message handler for listener-based fallback
        static void HandleSKEEMessage(SKSE::MessagingInterface::Message* msg);

    private:
        SKEEInterface() = default;

        bool TryDispatch(const char* pluginName);
        void ExtractInterfaces(IInterfaceMap* map);

        IBodyMorphInterface* bodyMorphIntfc_ = nullptr;
        IOverrideInterface* overrideIntfc_ = nullptr;
    };

}  // namespace PNS
