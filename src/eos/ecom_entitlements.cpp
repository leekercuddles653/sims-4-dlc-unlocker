#include "scream_api/scream_api.hpp"

#include <sdk/eos_ecom.h>

using namespace scream_api;

static Vector<String> entitlements;

DLL_EXPORT(void) EOS_Ecom_QueryEntitlements(
    EOS_HEcom Handle,
    const EOS_Ecom_QueryEntitlementsOptions* Options,
    void* ClientData,
    const EOS_Ecom_OnQueryEntitlementsCallback CompletionDelegate
) {
    GET_ORIGINAL_FUNCTION(EOS_Ecom_QueryEntitlements)

    static Set<String> entitlement_set;

    logger->info("❓ Game requested {} entitlements:", Options->EntitlementNameCount);

    for (uint32_t i = 0; i < Options->EntitlementNameCount; i++) {
        const auto id = Options->EntitlementNames[i];
        logger->info("  ❔ {}", id);

        if (config.entitlements.unlock_all) {
            entitlement_set.insert(id);
        }
    }

    struct Container {
        void* ClientData;
        EOS_Ecom_OnQueryEntitlementsCallback CompletionDelegate;
    };

    EOS_Ecom_QueryEntitlements_o(Handle, Options, new Container{ ClientData, CompletionDelegate },
        [](const EOS_Ecom_QueryEntitlementsCallbackInfo* Data) {
            try {
                const auto container = static_cast<Container*>(Data->ClientData);

                // Manually inject entitlements
                for (auto& id: config.entitlements.inject) {
                    logger->debug("Adding entitlement from config: {}", id);
                    entitlement_set.insert(id);
                }

                // Automatically inject entitlements
                if (scream_api::namespace_id.empty()) {
                    logger->warn("EOS_Ecom_QueryEntitlements callback: namespace_id is not set");
                } else {
                    if (config.entitlements.auto_inject) {
                        nlohmann::json payload = {
                            { "query",     R"(query($namespace: String!) {
                        Catalog {
                            catalogOffers(
                                namespace: $namespace
                                params: {
                                    count: 1000,
                                }
                            ) {
                                elements {
                                    items {
                                        id
                                    }
                                }
                            }
                        }
                    })" },
                            { "variables", {{ "namespace", scream_api::namespace_id }}}
                        };

                        const auto res = cpr::Post(
                            cpr::Url{ "https://graphql.epicgames.com/graphql" },
                            cpr::Header{{ "content-type", "application/json" }},
                            cpr::Body{ payload.dump() }
                        );

                        if (res.status_code == cpr::status::HTTP_OK) {
                            const auto json = nlohmann::json::parse(res.text);

                            logger->debug("Response json:\n{}", json.dump(2));

                            const auto elements = json["data"]["Catalog"]["catalogOffers"]["elements"];

                            for (const auto& element: elements) {
                                for (const auto& items: element) {
                                    for (const auto& item: items) {
                                        String id(item["id"]);
                                        logger->debug("Adding auto-fetched entitlement: {}", id);
                                        entitlement_set.insert(id);
                                    }
                                }
                            }
                        } else {
                            logger->error(
                                "Failed to automatically fetch entitlement ids. "
                                "Status code: {}. Text: {}", res.status_code, res.text
                            );
                        }
                    }
                }

                entitlements = std::vector(entitlement_set.begin(), entitlement_set.end());

                logger->info("🍀 ScreamAPI prepared {} entitlements:", entitlements.size());
                for (const auto& entitlement: entitlements) {
                    logger->info("  ✅ {}", entitlement);
                }

                const_cast<EOS_Ecom_QueryEntitlementsCallbackInfo*>(Data)->ResultCode = EOS_EResult::EOS_Success;
                const_cast<EOS_Ecom_QueryEntitlementsCallbackInfo*>(Data)->ClientData = container->ClientData;

                container->CompletionDelegate(Data);

                delete container;
            } catch (const Exception& ex) {
                util::panic("EOS_Ecom_QueryEntitlements callback error: {}", ex.what());
            }
        }
    );
}

DLL_EXPORT(uint32_t) EOS_Ecom_GetEntitlementsCount(
    EOS_HEcom,
    const EOS_Ecom_GetEntitlementsCountOptions*
) {
    const auto count = entitlements.size();

    logger->debug("Responding with the count of {} entitlements", count);

    return count;
}

DLL_EXPORT(EOS_EResult) EOS_Ecom_CopyEntitlementByIndex(
    EOS_HEcom,
    const EOS_Ecom_CopyEntitlementByIndexOptions* Options,
    EOS_Ecom_Entitlement** OutEntitlement
) {
    const auto index = Options->EntitlementIndex;
    if (index < 0 or index >= entitlements.size()) {
        logger->warn(
            "Game requested invalid entitlement index: {}. Max size: {}",
            index, entitlements.size()
        );

        return EOS_EResult::EOS_NotFound;
    }

    const auto id = entitlements[index].c_str();

    logger->debug("Copying the entitlement: {} at index: {}", id, index);

    *OutEntitlement = new EOS_Ecom_Entitlement{
        .ApiVersion = EOS_ECOM_ENTITLEMENT_API_LATEST,
        .EntitlementName = id,
        .EntitlementId = id,
        .CatalogItemId = id,
        .ServerIndex = -1,
        .bRedeemed = true, // TODO: Make it configurable?
        .EndTimestamp = -1,
    };

    return EOS_EResult::EOS_Success;
}

DLL_EXPORT(void) EOS_Ecom_Entitlement_Release(EOS_Ecom_Entitlement* Entitlement) {
    if (Entitlement) {
        logger->debug("Freeing a copy of the entitlement: {}", Entitlement->EntitlementName);
        delete Entitlement;
    } else {
        logger->warn("Game attempted to free a null entitlement");
    }
}
