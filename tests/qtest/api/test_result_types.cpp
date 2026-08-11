/*
 * test_result_types.cpp — QTest unit tests for Result<T> and API types
 *
 * New tests for P0 (API layer coverage).
 */

#include <QtTest>
#include <string>
#include "pv/api/types.h"

using pv::api::Result;
using pv::api::Error;
using pv::api::ErrorCode;
using pv::api::WorkMode;
using pv::api::CaptureState;
using pv::api::CollectMode;
using pv::api::ChannelType;
using pv::api::TriggerSlope;
using pv::api::TriggerSource;
using pv::api::Coupling;
using pv::api::GlitchFilterMode;
using pv::api::ServiceEvent;

class TestResultTypes : public QObject {
    Q_OBJECT
private slots:
    void SuccessHoldsValue();
    void FailHoldsError();
    void OkReturnsTrueForSuccess();
    void OkReturnsFalseForFail();
    void OperatorBoolTrueForSuccess();
    void OperatorBoolFalseForFail();
    void ValueReturnsStoredValue();
    void ErrorReturnsCode();
    void ErrorReturnsMessage();
    void VoidResultSuccess();
    void VoidResultFail();
    void ServiceEventTopicMapping();
    void MultipleResultsIndependent();
};

void TestResultTypes::SuccessHoldsValue() {
    auto r = Result<int>::Success(42);
    QVERIFY(r.ok());
    QCOMPARE(r.value(), 42);
}
void TestResultTypes::FailHoldsError() {
    auto r = Result<int>::Fail(ErrorCode::InvalidRequest, "bad");
    QVERIFY(!r.ok());
    QCOMPARE(r.error().code, ErrorCode::InvalidRequest);
    QCOMPARE(r.error().message, std::string("bad"));
}
void TestResultTypes::OkReturnsTrueForSuccess() {
    auto r = Result<int>::Success(0);
    QVERIFY(r.ok());
}
void TestResultTypes::OkReturnsFalseForFail() {
    auto r = Result<int>::Fail(ErrorCode::InternalError, "err");
    QVERIFY(!r.ok());
}
void TestResultTypes::OperatorBoolTrueForSuccess() {
    auto r = Result<int>::Success(1);
    QVERIFY(static_cast<bool>(r));
}
void TestResultTypes::OperatorBoolFalseForFail() {
    auto r = Result<int>::Fail(ErrorCode::NoData, "nd");
    QVERIFY(!static_cast<bool>(r));
}
void TestResultTypes::ValueReturnsStoredValue() {
    auto r = Result<int>::Success(99);
    QCOMPARE(r.value(), 99);
}
void TestResultTypes::ErrorReturnsCode() {
    auto r = Result<int>::Fail(ErrorCode::DeviceError, "de");
    QCOMPARE(r.error().code, ErrorCode::DeviceError);
}
void TestResultTypes::ErrorReturnsMessage() {
    auto r = Result<int>::Fail(ErrorCode::ConfigInvalid, "ci");
    QCOMPARE(r.error().message, std::string("ci"));
}
void TestResultTypes::VoidResultSuccess() {
    auto r = Result<void>::Success();
    QVERIFY(r.ok());
    QVERIFY(static_cast<bool>(r));
}
void TestResultTypes::VoidResultFail() {
    auto r = Result<void>::Fail(ErrorCode::SessionBusy, "sb");
    QVERIFY(!r.ok());
    QCOMPARE(r.error().code, ErrorCode::SessionBusy);
}
void TestResultTypes::ServiceEventTopicMapping() {
    using pv::api::service_event_topic;
    QCOMPARE(service_event_topic(ServiceEvent::CaptureStateChanged), "capture_state");
    QCOMPARE(service_event_topic(ServiceEvent::DataUpdated), "data_updated");
    QCOMPARE(service_event_topic(ServiceEvent::DeviceListUpdated), "device_list");
    QCOMPARE(service_event_topic(ServiceEvent::DecodeDone), "decode");
    QCOMPARE(service_event_topic(ServiceEvent::ErrorOccurred), "error");
    QCOMPARE(service_event_topic(ServiceEvent::ViewZoomFit), "view");
}
void TestResultTypes::MultipleResultsIndependent() {
    auto r1 = Result<int>::Success(1);
    auto r2 = Result<int>::Success(2);
    auto r3 = Result<int>::Fail(ErrorCode::NoData, "nd");
    QCOMPARE(r1.value(), 1);
    QCOMPARE(r2.value(), 2);
    QVERIFY(!r3.ok());
    QCOMPARE(r3.error().code, ErrorCode::NoData);
}

QTEST_MAIN(TestResultTypes)
#include "test_result_types.moc"
