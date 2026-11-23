#pragma once
#include <cstdint>
#include <string>

namespace HoshimiLocal::PhotoRequests {
    // Observers receive unmodified server responses, never the unlocked UI collections.
    void CaptureInitial(void* response);
    void CaptureUpdate(void* updatedData);
    void CaptureAvailability(void* response);
    void ApplyActivityVisualOutfits(void* progress);
    void ApplyDeckVisualOutfit(void* deckPosition);
    void ApplyLiveVisualOutfits(void* liveMessage);
    using CostumeValidator = bool (*)(void* specifier, void* costume, void* method);
    using HairValidator = bool (*)(void* hair, void* costume, void* method);
    void SetValidators(CostumeValidator costume, HairValidator hair);
    void SetCostumeRoomCharacter(const std::string& characterId);
    void* FindOwnedPhotoEntryOutfit(const std::string& characterId, void* selectedOutfit, void* costumeSpecifier = nullptr);

    class PreparedRequest {
    public:
        explicit PreparedRequest(void* original);
        PreparedRequest(void* request, uint32_t handle);
        PreparedRequest(const PreparedRequest&) = delete;
        PreparedRequest& operator=(const PreparedRequest&) = delete;
        PreparedRequest(PreparedRequest&& other) noexcept;
        ~PreparedRequest();
        void* get() const;
    private:
        void* request_;
        uint32_t handle_{};
    };

    PreparedRequest Prepare(void* request);
}
