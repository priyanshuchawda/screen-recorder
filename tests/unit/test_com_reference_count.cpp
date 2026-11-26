#include <gtest/gtest.h>

#include <atomic>
#include "controller/com_reference_count.h"

namespace {

class FakeUnknown final : public IUnknown {
public:
    explicit FakeUnknown(ULONG initial_references) : references_(initial_references) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown) return E_NOINTERFACE;
        *object = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return references_.fetch_sub(1, std::memory_order_relaxed) - 1;
    }

private:
    std::atomic<ULONG> references_;
};

TEST(ComReferenceCountTest, PoolOwnedSampleReferenceIsNotExternal) {
    FakeUnknown sample(1); // one pool ComPtr; helper adds its probe reference
    EXPECT_FALSE(sr::has_external_com_references(&sample, 1));

    sample.AddRef(); // simulate an outstanding sink-writer reference
    EXPECT_TRUE(sr::has_external_com_references(&sample, 1));
    sample.Release();
}

TEST(ComReferenceCountTest, SampleOwnedBufferReferenceIsIncludedInBaseline) {
    FakeUnknown buffer(2); // pool ComPtr plus the sample's AddBuffer reference
    EXPECT_FALSE(sr::has_external_com_references(&buffer, 2));

    buffer.AddRef(); // simulate a downstream holder of the buffer
    EXPECT_TRUE(sr::has_external_com_references(&buffer, 2));
    buffer.Release();
}

TEST(ComReferenceCountTest, NullObjectHasNoExternalReferences) {
    EXPECT_FALSE(sr::has_external_com_references(nullptr, 0));
}

} // namespace
