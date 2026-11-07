#pragma once
#include <cstdint>

namespace HoshimiLocal::PhotoRequests {
    // Observers receive unmodified server responses, never the unlocked UI collections.
    void CaptureInitial(void* response);
    void CaptureUpdate(void* updatedData);
    void CaptureAvailability(void* response);
    void ObserveUnlock(void* item, bool unlocked);
    using CostumeValidator = bool (*)(void* specifier, void* costume, void* method);
    using HairValidator = bool (*)(void* hair, void* costume, void* method);
    void SetValidators(CostumeValidator costume, HairValidator hair);

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
