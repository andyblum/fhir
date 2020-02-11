// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/fhir/fhir_path/fhir_path.h"

#include "google/protobuf/message.h"
#include "google/protobuf/text_format.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/str_cat.h"
#include "absl/time/civil_time.h"
#include "absl/time/time.h"
#include "google/fhir/proto_util.h"
#include "google/fhir/status/status.h"
#include "google/fhir/testutil/proto_matchers.h"
#include "proto/r4/core/codes.pb.h"
#include "proto/r4/core/datatypes.pb.h"
#include "proto/r4/core/resources/encounter.pb.h"
#include "proto/r4/core/resources/medication_knowledge.pb.h"
#include "proto/r4/core/resources/observation.pb.h"
#include "proto/r4/core/resources/parameters.pb.h"
#include "proto/r4/core/resources/structure_definition.pb.h"
#include "proto/r4/core/resources/value_set.pb.h"
#include "proto/r4/uscore.pb.h"
#include "proto/r4/uscore_codes.pb.h"
#include "tensorflow/core/lib/core/errors.h"

namespace google {
namespace fhir {
namespace fhir_path {

namespace {

using r4::core::Boolean;
using r4::core::Code;
using r4::core::CodeableConcept;
using r4::core::DateTime;
using r4::core::Decimal;
using r4::core::Encounter;
using r4::core::Integer;
using r4::core::MedicationKnowledge_Kinetics;
using r4::core::Observation;
using r4::core::Parameters;
using r4::core::Period;
using r4::core::Quantity;
using r4::core::SimpleQuantity;
using r4::core::String;
using r4::core::StructureDefinition;
using r4::core::ValueSet;
using r4::uscore::BirthSexValueSet;
using r4::uscore::USCorePatientProfile;

using testutil::EqualsProto;

using ::google::protobuf::FieldDescriptor;
using ::google::protobuf::Message;
using testing::ElementsAreArray;
using testing::IsEmpty;
using testing::UnorderedElementsAreArray;

static ::google::protobuf::TextFormat::Parser parser;  // NOLINT

MATCHER(EvalsToEmpty, "") {
  return arg.ok() && arg.ValueOrDie().GetMessages().empty();
}

// Matcher for StatusOr<EvaluationResult> that checks to see that the evaluation
// succeeded and evaluated to a single boolean with value of false.
//
// NOTE: Not(EvalsToFalse()) is not the same as EvalsToTrue() as the former
// will match cases where evaluation fails.
MATCHER(EvalsToFalse, "") {
  if (!arg.ok()) {
    *result_listener << "evaluation error: " << arg.status().error_message();
    return false;
  }

  StatusOr<bool> result = arg.ValueOrDie().GetBoolean();
  if (!result.ok()) {
    *result_listener << "did not resolve to a boolean: "
                     << result.status().error_message();
    return false;
  }

  return !result.ValueOrDie();
}

// Matcher for StatusOr<EvaluationResult> that checks to see that the evaluation
// succeeded and evaluated to a single boolean with value of true.
//
// NOTE: Not(EvalsToTrue()) is not the same as EvalsToFalse() as the former
// will match cases where evaluation fails.
MATCHER(EvalsToTrue, "") {
  if (!arg.ok()) {
    *result_listener << "evaluation error: " << arg.status().error_message();
    return false;
  }

  StatusOr<bool> result = arg.ValueOrDie().GetBoolean();
  if (!result.ok()) {
    *result_listener << "did not resolve to a boolean: "
                     << result.status().error_message();
    return false;
  }

  return result.ValueOrDie();
}

template <typename T>
T ParseFromString(const std::string& str) {
  google::protobuf::TextFormat::Parser parser;
  parser.AllowPartialMessage(true);
  T t;
  EXPECT_TRUE(parser.ParseFromString(str, &t));
  return t;
}

// TODO: Templatize methods to work with both STU3 and R4
Encounter ValidEncounter() {
  return ParseFromString<Encounter>(R"proto(
    status { value: TRIAGED }
    id { value: "123" }
    period {
      start: { value_us: 1556750153000 timezone: "America/Los_Angeles" }
    }
    status_history { status { value: ARRIVED } }
  )proto");
}

Observation ValidObservation() {
  return ParseFromString<Observation>(R"proto(
    status { value: FINAL }
    code {
      coding {
        system { value: "foo" }
        code { value: "bar" }
      }
    }
    id { value: "123" }
  )proto");
}

ValueSet ValidValueSet() {
  return ParseFromString<ValueSet>(R"proto(
    url { value: "http://example.com/valueset" }
  )proto");
}

USCorePatientProfile ValidUsCorePatient() {
  return ParseFromString<USCorePatientProfile>(R"proto(
    identifier {
      system { value: "foo" },
      value: { value: "http://example.com/patient" }
    }
  )proto");
}

template <typename T>
StatusOr<EvaluationResult> Evaluate(const T& message,
    const std::string& expression) {
    FHIR_ASSIGN_OR_RETURN(auto compiled_expression,
      CompiledExpression::Compile(message.GetDescriptor(), expression));

  return compiled_expression.Evaluate(message);
}

StatusOr<EvaluationResult> Evaluate(
    const std::string& expression) {
  // FHIRPath assumes a resource object during evaluation, so we use an
  // encounter as a placeholder.
  Encounter test_encounter = ValidEncounter();
  return Evaluate(test_encounter, expression);
}

StatusOr<std::string> EvaluateStringExpressionWithStatus(
    const std::string& expression) {
  FHIR_ASSIGN_OR_RETURN(EvaluationResult result, Evaluate(expression));

  return result.GetString();
}

StatusOr<bool> EvaluateBoolExpressionWithStatus(const std::string& expression) {
  FHIR_ASSIGN_OR_RETURN(EvaluationResult result, Evaluate(expression));

  return result.GetBoolean();
}

bool EvaluateBoolExpression(const std::string& expression) {
  return EvaluateBoolExpressionWithStatus(expression).ValueOrDie();
}

DateTime ToDateTime(const absl::CivilSecond& civil_time,
                    const absl::TimeZone& zone,
                    const DateTime::Precision& precision) {
  DateTime date_time;
  date_time.set_value_us(absl::ToUnixMicros(absl::FromCivil(civil_time, zone)));
  date_time.set_timezone(zone.name());
  date_time.set_precision(precision);
  return date_time;
}

// Helper to evaluate boolean expressions on periods with the
// given start and end times.
bool EvaluateOnPeriod(const CompiledExpression& expression,
                      const DateTime& start, const DateTime& end) {
  Period period;
  *period.mutable_start() = start;
  *period.mutable_end() = end;
  EvaluationResult result = expression.Evaluate(period).ValueOrDie();
  return result.GetBoolean().ValueOrDie();
}

// TODO: Templatize tests to work with both STU3 and R4
TEST(FhirPathTest, TestExternalConstants) {
  EvaluationResult ucum_evaluation_result = Evaluate("%ucum").ValueOrDie();
  String ucum_expected_result =
      ParseFromString<String>("value: 'http://unitsofmeasure.org'");

  EXPECT_THAT(ucum_evaluation_result.GetMessages(),
              UnorderedElementsAreArray({EqualsProto(ucum_expected_result)}));

  EvaluationResult sct_evaluation_result = Evaluate("%sct").ValueOrDie();
  String sct_expected_result =
      ParseFromString<String>("value: 'http://snomed.info/sct'");

  EXPECT_THAT(sct_evaluation_result.GetMessages(),
              UnorderedElementsAreArray({EqualsProto(sct_expected_result)}));

  EvaluationResult loinc_evaluation_result = Evaluate("%loinc").ValueOrDie();
  String loinc_expected_result =
      ParseFromString<String>("value: 'http://loinc.org'");

  EXPECT_THAT(loinc_evaluation_result.GetMessages(),
              UnorderedElementsAreArray({EqualsProto(loinc_expected_result)}));

  EXPECT_FALSE(Evaluate("%unknown").ok());
}

TEST(FhirPathTest, TestExternalConstantsContext) {
  Encounter test_encounter = ValidEncounter();

  auto result = CompiledExpression::Compile(Encounter::descriptor(), "%context")
                    .ValueOrDie()
                    .Evaluate(test_encounter)
                    .ValueOrDie();
  EXPECT_THAT(result.GetMessages(),
              UnorderedElementsAreArray({EqualsProto(test_encounter)}));
}

TEST(FhirPathTest, TestExternalConstantsContextReferenceInExpressionParam) {
  Encounter test_encounter = ValidEncounter();

  auto result = CompiledExpression::Compile(Encounter::descriptor(),
                                            "status.select(%context)")
                    .ValueOrDie()
                    .Evaluate(test_encounter)
                    .ValueOrDie();
  EXPECT_THAT(result.GetMessages(),
              UnorderedElementsAreArray({EqualsProto(test_encounter)}));
}

TEST(FhirPathTest, TestMalformed) {
  auto expr = CompiledExpression::Compile(Encounter::descriptor(),
                                          "expression->not->valid");

  EXPECT_FALSE(expr.ok());
}

TEST(FhirPathTest, TestGetDirectChild) {
  auto expr = CompiledExpression::Compile(Encounter::descriptor(), "status")
                  .ValueOrDie();

  Encounter test_encounter = ValidEncounter();

  EvaluationResult result = expr.Evaluate(test_encounter).ValueOrDie();

  ASSERT_EQ(1, result.GetMessages().size());

  const Message* status = result.GetMessages()[0];

  EXPECT_THAT(*status, EqualsProto(test_encounter.status()));
}

TEST(FhirPathTest, TestGetGrandchild) {
  auto expr =
      CompiledExpression::Compile(Encounter::descriptor(), "period.start")
          .ValueOrDie();

  Encounter test_encounter = ValidEncounter();

  EvaluationResult result = expr.Evaluate(test_encounter).ValueOrDie();

  ASSERT_EQ(1, result.GetMessages().size());

  const Message* status = result.GetMessages()[0];

  EXPECT_THAT(*status, EqualsProto(test_encounter.period().start()));
}

TEST(FhirPathTest, TestGetEmptyGrandchild) {
  auto expr = CompiledExpression::Compile(Encounter::descriptor(), "period.end")
                  .ValueOrDie();

  Encounter test_encounter = ValidEncounter();

  EvaluationResult result = expr.Evaluate(test_encounter).ValueOrDie();

  EXPECT_EQ(0, result.GetMessages().size());
}

TEST(FhirPathTest, TestFieldExists) {
  Encounter test_encounter = ValidEncounter();
  test_encounter.mutable_class_value()->mutable_display()->set_value("foo");

  auto root_expr =
      CompiledExpression::Compile(Encounter::descriptor(), "period")
          .ValueOrDie();
  EvaluationResult root_result =
      root_expr.Evaluate(test_encounter).ValueOrDie();
  EXPECT_THAT(
      root_result.GetMessages(),
      UnorderedElementsAreArray({EqualsProto(test_encounter.period())}));

  // Tests the conversion from camelCase to snake_case
  auto camel_case_expr =
      CompiledExpression::Compile(Encounter::descriptor(), "statusHistory")
          .ValueOrDie();
  EvaluationResult camel_case_result =
      camel_case_expr.Evaluate(test_encounter).ValueOrDie();
  EXPECT_THAT(camel_case_result.GetMessages(),
              UnorderedElementsAreArray(
                  {EqualsProto(test_encounter.status_history(0))}));

  // Test that the json_name field annotation is used when searching for a
  // field.
  auto json_name_alias_expr =
      CompiledExpression::Compile(Encounter::descriptor(), "class")
          .ValueOrDie();
  EvaluationResult json_name_alias_result =
      json_name_alias_expr.Evaluate(test_encounter).ValueOrDie();
  EXPECT_THAT(
      json_name_alias_result.GetMessages(),
      UnorderedElementsAreArray({EqualsProto(test_encounter.class_value())}));
}

TEST(FhirPathTest, TestNoSuchField) {
  auto root_expr =
      CompiledExpression::Compile(Encounter::descriptor(), "bogusrootfield");

  EXPECT_FALSE(root_expr.ok());
  EXPECT_NE(root_expr.status().error_message().find("bogusrootfield"),
            std::string::npos);

  auto child_expr = CompiledExpression::Compile(Encounter::descriptor(),
                                                "period.boguschildfield");

  EXPECT_FALSE(child_expr.ok());
  EXPECT_NE(child_expr.status().error_message().find("boguschildfield"),
            std::string::npos);

  EXPECT_THAT(Evaluate(ValidEncounter(), "(period | status).boguschildfield"),
              EvalsToEmpty());
}

TEST(FhirPathTest, TestNoSuchFunction) {
  auto root_expr = CompiledExpression::Compile(Encounter::descriptor(),
                                               "period.bogusfunction()");

  EXPECT_FALSE(root_expr.ok());
  EXPECT_NE(root_expr.status().error_message().find("bogusfunction"),
            std::string::npos);
}

TEST(FhirPathTest, TestFunctionTopLevelInvocation) {
  EXPECT_TRUE(EvaluateBoolExpression("exists()"));
}

TEST(FhirPathTest, TestFunctionExists) {
  auto expr = CompiledExpression::Compile(Encounter::descriptor(),
                                          "period.start.exists()")
                  .ValueOrDie();

  Encounter test_encounter = ValidEncounter();

  EvaluationResult result = expr.Evaluate(test_encounter).ValueOrDie();

  EXPECT_TRUE(result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestFunctionExistsNegation) {
  auto expr = CompiledExpression::Compile(Encounter::descriptor(),
                                          "period.start.exists().not()")
                  .ValueOrDie();

  Encounter test_encounter = ValidEncounter();

  EvaluationResult result = expr.Evaluate(test_encounter).ValueOrDie();

  EXPECT_FALSE(result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestFunctionNotExists) {
  auto expr = CompiledExpression::Compile(Encounter::descriptor(),
                                          "period.end.exists()")
                  .ValueOrDie();

  Encounter test_encounter = ValidEncounter();

  EvaluationResult result = expr.Evaluate(test_encounter).ValueOrDie();

  EXPECT_FALSE(result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestFunctionNotExistsNegation) {
  auto expr = CompiledExpression::Compile(Encounter::descriptor(),
                                          "period.end.exists().not()")
                  .ValueOrDie();

  Encounter test_encounter = ValidEncounter();

  EvaluationResult result = expr.Evaluate(test_encounter).ValueOrDie();

  EXPECT_TRUE(result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestFunctionHasValue) {
  auto expr = CompiledExpression::Compile(Encounter::descriptor(),
                                          "period.start.hasValue()")
                  .ValueOrDie();

  Encounter test_encounter = ValidEncounter();

  EvaluationResult result = expr.Evaluate(test_encounter).ValueOrDie();

  EXPECT_TRUE(result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestLogicalValueFieldExists) {
  // The logical .value field on primitives should return the primitive itself.
  auto expr = CompiledExpression::Compile(Quantity::descriptor(),
                                          "value.value.exists()")
                  .ValueOrDie();
  Quantity quantity;
  quantity.mutable_value()->set_value("100");
  EvaluationResult result = expr.Evaluate(quantity).ValueOrDie();
  EXPECT_TRUE(result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestFunctionHasValueNegation) {
  auto expr = CompiledExpression::Compile(Encounter::descriptor(),
                                          "period.start.hasValue().not()")
                  .ValueOrDie();

  Encounter test_encounter = ValidEncounter();

  EvaluationResult has_value_result =
      expr.Evaluate(test_encounter).ValueOrDie();
  EXPECT_FALSE(has_value_result.GetBoolean().ValueOrDie());

  test_encounter.mutable_period()->clear_start();
  EvaluationResult no_value_result = expr.Evaluate(test_encounter).ValueOrDie();
  EXPECT_TRUE(no_value_result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestFunctionChildren) {
  StructureDefinition structure_definition =
      ParseFromString<StructureDefinition>(R"proto(
        name { value: "foo" }
        context { expression { value: "bar" } }
        snapshot { element { label { value: "snapshot" } } }
        differential { element { label { value: "differential" } } }
      )proto");

  EXPECT_THAT(
      Evaluate(structure_definition, "children()").ValueOrDie().GetMessages(),
      UnorderedElementsAreArray(
          {EqualsProto(structure_definition.name()),
           EqualsProto(structure_definition.context(0)),
           EqualsProto(structure_definition.snapshot()),
           EqualsProto(structure_definition.differential())}));

  EXPECT_THAT(
      Evaluate(structure_definition, "children().element")
          .ValueOrDie().GetMessages(),
      UnorderedElementsAreArray(
          {EqualsProto(structure_definition.snapshot().element(0)),
           EqualsProto(structure_definition.differential().element(0))}));
}

TEST(FhirPathTest, TestFunctionContains) {
  // Wrong number and/or types of arguments.
  EXPECT_FALSE(EvaluateBoolExpressionWithStatus("'foo'.contains()").ok());
  EXPECT_FALSE(EvaluateBoolExpressionWithStatus("'foo'.contains(1)").ok());
  EXPECT_FALSE(
      EvaluateBoolExpressionWithStatus("'foo'.contains('a', 'b')").ok());

  EXPECT_TRUE(EvaluateBoolExpression("'foo'.contains('')"));
  EXPECT_TRUE(EvaluateBoolExpression("'foo'.contains('o')"));
  EXPECT_TRUE(EvaluateBoolExpression("'foo'.contains('foo')"));
  EXPECT_FALSE(EvaluateBoolExpression("'foo'.contains('foob')"));
  EXPECT_TRUE(EvaluateBoolExpression("''.contains('')"));
  EXPECT_FALSE(EvaluateBoolExpression("''.contains('foo')"));

  EXPECT_THAT(Evaluate("{}.contains('foo')"), EvalsToEmpty());
}

TEST(FhirPathTest, TestFunctionStartsWith) {
  // Missing argument
  EXPECT_FALSE(EvaluateBoolExpressionWithStatus("'foo'.startsWith()").ok());

  // Too many arguments
  EXPECT_FALSE(
      EvaluateBoolExpressionWithStatus("'foo'.startsWith('foo', 'foo')").ok());

  // Wrong argument type
  EXPECT_FALSE(EvaluateBoolExpressionWithStatus("'foo'.startsWith(1)").ok());
  EXPECT_FALSE(EvaluateBoolExpressionWithStatus("'foo'.startsWith(1.0)").ok());
  EXPECT_FALSE(EvaluateBoolExpressionWithStatus(
                   "'foo'.startsWith(@2015-02-04T14:34:28Z)")
                   .ok());
  EXPECT_FALSE(EvaluateBoolExpressionWithStatus("'foo'.startsWith(true)").ok());

  // Function does not exist for non-string type
  EXPECT_FALSE(EvaluateBoolExpressionWithStatus("1.startsWith(1)").ok());
  EXPECT_FALSE(EvaluateBoolExpressionWithStatus("1.startsWith('1')").ok());

  // Basic cases
  EXPECT_TRUE(EvaluateBoolExpression("''.startsWith('')"));
  EXPECT_TRUE(EvaluateBoolExpression("'foo'.startsWith('')"));
  EXPECT_TRUE(EvaluateBoolExpression("'foo'.startsWith('f')"));
  EXPECT_TRUE(EvaluateBoolExpression("'foo'.startsWith('foo')"));
  EXPECT_FALSE(EvaluateBoolExpression("'foo'.startsWith('foob')"));
}

TEST(FhirPathTest, TestFunctionStartsWithSelfReference) {
  auto expr = CompiledExpression::Compile(
                  Observation::descriptor(),
                  "code.coding.code.startsWith(code.coding.code)")
                  .ValueOrDie();

  Observation test_observation = ValidObservation();

  EvaluationResult contains_result =
      expr.Evaluate(test_observation).ValueOrDie();
  EXPECT_TRUE(contains_result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestFunctionStartsWithInvokedOnNonString) {
  auto expr = CompiledExpression::Compile(Observation::descriptor(),
                                          "code.startsWith('foo')")
                  .ValueOrDie();

  Observation test_observation = ValidObservation();

  EXPECT_FALSE(expr.Evaluate(test_observation).ok());
}

TEST(FhirPathTest, TestFunctionMatches) {
  EXPECT_THAT(Evaluate("{}.matches('')"), EvalsToEmpty());
  EXPECT_TRUE(EvaluateBoolExpression("''.matches('')"));
  EXPECT_TRUE(EvaluateBoolExpression("'a'.matches('a')"));
  EXPECT_FALSE(EvaluateBoolExpression("'abc'.matches('a')"));
  EXPECT_TRUE(EvaluateBoolExpression("'abc'.matches('...')"));
}

TEST(FhirPathTest, TestFunctionLength) {
  EXPECT_THAT(Evaluate("{}.length()"), EvalsToEmpty());
  EXPECT_TRUE(EvaluateBoolExpression("''.length() = 0"));
  EXPECT_TRUE(EvaluateBoolExpression("'abc'.length() = 3"));

  EXPECT_FALSE(Evaluate("3.length()").ok());
}

TEST(FhirPathTest, TestFunctionToInteger) {
  EXPECT_EQ(Evaluate("1.toInteger()")
                .ValueOrDie()
                .GetInteger()
                .ValueOrDie(),
            1);
  EXPECT_EQ(Evaluate("'2'.toInteger()")
                .ValueOrDie()
                .GetInteger()
                .ValueOrDie(),
            2);

  EXPECT_THAT(Evaluate("(3.3).toInteger()"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("'a'.toInteger()"), EvalsToEmpty());

  EXPECT_FALSE(Evaluate("(1 | 2).toInteger()").ok());
}

TEST(FhirPathTest, TestFunctionToString) {
  EXPECT_EQ(EvaluateStringExpressionWithStatus("1.toString()").ValueOrDie(),
            "1");
  EXPECT_EQ(EvaluateStringExpressionWithStatus("1.1.toString()").ValueOrDie(),
            "1.1");
  EXPECT_EQ(EvaluateStringExpressionWithStatus("'foo'.toString()").ValueOrDie(),
            "foo");
  EXPECT_EQ(EvaluateStringExpressionWithStatus("true.toString()").ValueOrDie(),
            "true");
  EXPECT_THAT(Evaluate("{}.toString()"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("toString()"), EvalsToEmpty());
  EXPECT_FALSE(Evaluate("(1 | 2).toString()").ok());
}

TEST(FhirPathTest, TestFunctionTrace) {
  EXPECT_TRUE(EvaluateBoolExpression("true.trace('debug')"));
  EXPECT_THAT(Evaluate("{}.trace('debug')"), EvalsToEmpty());
}

TEST(FhirPathTest, TestFunctionHasValueComplex) {
  auto expr =
      CompiledExpression::Compile(Encounter::descriptor(), "period.hasValue()")
          .ValueOrDie();

  Encounter test_encounter = ValidEncounter();

  EvaluationResult result = expr.Evaluate(test_encounter).ValueOrDie();

  // hasValue should return false for non-primitive types.
  EXPECT_FALSE(result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestFunctionEmpty) {
  EXPECT_TRUE(EvaluateBoolExpression("{}.empty()"));
  EXPECT_FALSE(EvaluateBoolExpression("true.empty()"));
  EXPECT_FALSE(EvaluateBoolExpression("(false | true).empty()"));
}

TEST(FhirPathTest, TestFunctionCount) {
  EXPECT_EQ(Evaluate("{}.count()").ValueOrDie()
                .GetInteger().ValueOrDie(),
            0);
  EXPECT_EQ(Evaluate("'a'.count()").ValueOrDie()
                .GetInteger().ValueOrDie(),
            1);
  EXPECT_EQ(Evaluate("('a' | 1).count()").ValueOrDie()
                .GetInteger().ValueOrDie(),
            2);
}

TEST(FhirPathTest, TestFunctionFirst) {
  EXPECT_THAT(Evaluate("{}.first()"), EvalsToEmpty());
  EXPECT_TRUE(EvaluateBoolExpression("true.first()"));
  EXPECT_TRUE(Evaluate("(false | true).first()").ok());
}

TEST(FhirPathTest, TestFunctionTail) {
  EXPECT_THAT(Evaluate("{}.tail()"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("true.tail()"), EvalsToEmpty());
  EXPECT_TRUE(EvaluateBoolExpression("true.combine(true).tail()"));
}

TEST(FhirPathTest, TestFunctionAsPrimitives) {
  EXPECT_THAT(Evaluate("{}.as(Boolean)"), EvalsToEmpty());

  EXPECT_TRUE(EvaluateBoolExpression("true.as(Boolean)"));
  EXPECT_THAT(Evaluate("true.as(Decimal)"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("true.as(Integer)"), EvalsToEmpty());

  EXPECT_EQ(Evaluate("1.as(Integer)").ValueOrDie().GetInteger().ValueOrDie(),
            1);
  EXPECT_THAT(Evaluate("1.as(Decimal)"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("1.as(Boolean)"), EvalsToEmpty());

  EXPECT_EQ(Evaluate("1.1.as(Decimal)").ValueOrDie().GetDecimal().ValueOrDie(),
            "1.1");
  EXPECT_THAT(Evaluate("1.1.as(Integer)"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("1.1.as(Boolean)"), EvalsToEmpty());
}

TEST(FhirPathTest, TestFunctionAsResources) {
  Observation observation = ParseFromString<Observation>(R"proto()proto");

  EXPECT_THAT(Evaluate(observation, "$this.as(Boolean)"), EvalsToEmpty());
  EXPECT_THAT(Evaluate(observation, "$this.as(CodeableConcept)"),
              EvalsToEmpty());

  EvaluationResult as_observation_evaluation_result =
      Evaluate(observation, "$this.as(Observation)").ValueOrDie();
  EXPECT_THAT(as_observation_evaluation_result.GetMessages(),
              ElementsAreArray({EqualsProto(observation)}));;
}

TEST(FhirPathTest, TestOperatorAsPrimitives) {
  EXPECT_THAT(Evaluate("{} as Boolean"), EvalsToEmpty());

  EXPECT_TRUE(EvaluateBoolExpression("true as Boolean"));
  EXPECT_THAT(Evaluate("true as Decimal"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("true as Integer"), EvalsToEmpty());

  EXPECT_EQ(Evaluate("1 as Integer").ValueOrDie().GetInteger().ValueOrDie(), 1);
  EXPECT_THAT(Evaluate("1 as Decimal"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("1 as Boolean"), EvalsToEmpty());

  EXPECT_EQ(Evaluate("1.1 as Decimal").ValueOrDie().GetDecimal().ValueOrDie(),
            "1.1");
  EXPECT_THAT(Evaluate("1.1 as Integer"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("1.1 as Boolean"), EvalsToEmpty());
}

TEST(FhirPathTest, TestOperatorAsResources) {
  Observation observation = ParseFromString<Observation>(R"proto()proto");

  EXPECT_THAT(Evaluate(observation, "$this as Boolean"), EvalsToEmpty());
  EXPECT_THAT(Evaluate(observation, "$this as CodeableConcept"),
              EvalsToEmpty());

  EvaluationResult as_observation_evaluation_result =
      Evaluate(observation, "$this as Observation").ValueOrDie();
  EXPECT_THAT(as_observation_evaluation_result.GetMessages(),
              ElementsAreArray({EqualsProto(observation)}));
}

TEST(FhirPathTest, TestFunctionIsPrimitives) {
  EXPECT_THAT(Evaluate("{}.is(Boolean)"), EvalsToEmpty());

  EXPECT_TRUE(EvaluateBoolExpression("true.is(Boolean)"));
  EXPECT_FALSE(EvaluateBoolExpression("true.is(Decimal)"));
  EXPECT_FALSE(EvaluateBoolExpression("true.is(Integer)"));

  EXPECT_TRUE(EvaluateBoolExpression("1.is(Integer)"));
  EXPECT_FALSE(EvaluateBoolExpression("1.is(Decimal)"));
  EXPECT_FALSE(EvaluateBoolExpression("1.is(Boolean)"));

  EXPECT_TRUE(EvaluateBoolExpression("1.1.is(Decimal)"));
  EXPECT_FALSE(EvaluateBoolExpression("1.1.is(Integer)"));
  EXPECT_FALSE(EvaluateBoolExpression("1.1.is(Boolean)"));
}

TEST(FhirPathTest, TestFunctionIsResources) {
  Observation observation = ParseFromString<Observation>(R"proto()proto");

  EvaluationResult is_boolean_evaluation_result =
      CompiledExpression::Compile(CodeableConcept::descriptor(),
                                  "$this.is(Boolean)")
          .ValueOrDie()
          .Evaluate(observation)
          .ValueOrDie();
  EXPECT_FALSE(is_boolean_evaluation_result.GetBoolean().ValueOrDie());

  EvaluationResult is_codeable_concept_evaluation_result =
      CompiledExpression::Compile(CodeableConcept::descriptor(),
                                  "$this.is(CodeableConcept)")
          .ValueOrDie()
          .Evaluate(observation)
          .ValueOrDie();
  EXPECT_FALSE(is_codeable_concept_evaluation_result.GetBoolean().ValueOrDie());

  EvaluationResult is_observation_evaluation_result =
      CompiledExpression::Compile(CodeableConcept::descriptor(),
                                  "$this.is(Observation)")
          .ValueOrDie()
          .Evaluate(observation)
          .ValueOrDie();
  EXPECT_TRUE(is_observation_evaluation_result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestOperatorIsPrimitives) {
  EXPECT_THAT(Evaluate("{} is Boolean"), EvalsToEmpty());

  EXPECT_TRUE(EvaluateBoolExpression("true is Boolean"));
  EXPECT_FALSE(EvaluateBoolExpression("true is Decimal"));
  EXPECT_FALSE(EvaluateBoolExpression("true is Integer"));

  EXPECT_TRUE(EvaluateBoolExpression("1 is Integer"));
  EXPECT_FALSE(EvaluateBoolExpression("1 is Decimal"));
  EXPECT_FALSE(EvaluateBoolExpression("1 is Boolean"));

  EXPECT_TRUE(EvaluateBoolExpression("1.1 is Decimal"));
  EXPECT_FALSE(EvaluateBoolExpression("1.1 is Integer"));
  EXPECT_FALSE(EvaluateBoolExpression("1.1 is Boolean"));
}

TEST(FhirPathTest, TestOperatorIsResources) {
  Observation observation = ParseFromString<Observation>(R"proto()proto");

  EvaluationResult is_boolean_evaluation_result =
      CompiledExpression::Compile(CodeableConcept::descriptor(),
                                  "$this is Boolean")
          .ValueOrDie()
          .Evaluate(observation)
          .ValueOrDie();
  EXPECT_FALSE(is_boolean_evaluation_result.GetBoolean().ValueOrDie());

  EvaluationResult is_codeable_concept_evaluation_result =
      CompiledExpression::Compile(CodeableConcept::descriptor(),
                                  "$this is CodeableConcept")
          .ValueOrDie()
          .Evaluate(observation)
          .ValueOrDie();
  EXPECT_FALSE(is_codeable_concept_evaluation_result.GetBoolean().ValueOrDie());

  EvaluationResult is_observation_evaluation_result =
      CompiledExpression::Compile(CodeableConcept::descriptor(),
                                  "$this is Observation")
          .ValueOrDie()
          .Evaluate(observation)
          .ValueOrDie();
  EXPECT_TRUE(is_observation_evaluation_result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestFunctionTailMaintainsOrder) {
  CodeableConcept observation = ParseFromString<CodeableConcept>(R"proto(
    coding {
      system { value: "foo" }
      code { value: "abc" }
    }
    coding {
      system { value: "bar" }
      code { value: "def" }
    }
    coding {
      system { value: "foo" }
      code { value: "ghi" }
    }
  )proto");

  Code code_def = ParseFromString<Code>("value: 'def'");
  Code code_ghi = ParseFromString<Code>("value: 'ghi'");
  EvaluationResult evaluation_result =
      CompiledExpression::Compile(CodeableConcept::descriptor(),
                                  "coding.tail().code")
          .ValueOrDie()
          .Evaluate(observation)
          .ValueOrDie();
  EXPECT_THAT(evaluation_result.GetMessages(),
              ElementsAreArray({EqualsProto(code_def), EqualsProto(code_ghi)}));
}

TEST(FhirPathTest, TestUnion) {
  EXPECT_THAT(Evaluate("({} | {})"), EvalsToEmpty());

  EXPECT_TRUE(EvaluateBoolExpression("(true | {}) = true"));
  EXPECT_TRUE(EvaluateBoolExpression("(true | true) = true"));

  EXPECT_TRUE(EvaluateBoolExpression("(false | {}) = false"));
  EXPECT_TRUE(EvaluateBoolExpression("(false | false) = false"));
}

// TODO: Templatize tests to work with both STU3 and R4
TEST(FhirPathTest, TestUnionDeduplicationPrimitives) {
  EvaluationResult evaluation_result =
      Evaluate("true | false | 1 | 'foo' | 2 | 1 | 'foo'")
          .ValueOrDie();
  std::vector<const Message*> result = evaluation_result.GetMessages();

  Boolean true_proto = ParseFromString<Boolean>("value: true");
  Boolean false_proto = ParseFromString<Boolean>("value: false");
  Integer integer_1_proto = ParseFromString<Integer>("value: 1");
  Integer integer_2_proto = ParseFromString<Integer>("value: 2");
  String string_foo_proto = ParseFromString<String>("value: 'foo'");

  ASSERT_THAT(result,
              UnorderedElementsAreArray(
                  {EqualsProto(true_proto),
                   EqualsProto(false_proto),
                   EqualsProto(integer_1_proto),
                   EqualsProto(integer_2_proto),
                   EqualsProto(string_foo_proto)}));
}

TEST(FhirPathTest, TestUnionDeduplicationObjects) {
  Encounter test_encounter = ValidEncounter();

  EvaluationResult evaluation_result =
      CompiledExpression::Compile(Encounter::descriptor(),
                                  ("period | status | status | period"))
          .ValueOrDie()
          .Evaluate(test_encounter)
          .ValueOrDie();
  std::vector<const Message*> result = evaluation_result
          .GetMessages();

  ASSERT_THAT(result,
              UnorderedElementsAreArray(
                  {EqualsProto(test_encounter.status()),
                   EqualsProto(test_encounter.period())}));
}

// TODO: Templatize tests to work with both STU3 and R4
TEST(FhirPathTest, TestCombine) {
  EXPECT_THAT(Evaluate("{}.combine({})"), EvalsToEmpty());
  EXPECT_TRUE(EvaluateBoolExpression("true.combine({})"));
  EXPECT_TRUE(EvaluateBoolExpression("{}.combine(true)"));

  Boolean true_proto = ParseFromString<Boolean>("value: true");
  Boolean false_proto = ParseFromString<Boolean>("value: false");
  EvaluationResult evaluation_result =
      Evaluate("true.combine(true).combine(false)")
          .ValueOrDie();
  EXPECT_THAT(evaluation_result.GetMessages(),
              UnorderedElementsAreArray({EqualsProto(true_proto),
                                         EqualsProto(true_proto),
                                         EqualsProto(false_proto)}));
}

// TODO: Templatize tests to work with both STU3 and R4
TEST(FhirPathTest, TestIntersect) {
  EXPECT_THAT(Evaluate("{}.intersect({})"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("true.intersect({})"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("true.intersect(false)"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("{}.intersect(true)"), EvalsToEmpty());
  EXPECT_TRUE(EvaluateBoolExpression("true.intersect(true) = true"));
  EXPECT_TRUE(EvaluateBoolExpression("(true | false).intersect(true) = true"));

  EXPECT_TRUE(
      EvaluateBoolExpression("(true.combine(true)).intersect(true) = true)"));
  EXPECT_TRUE(
      EvaluateBoolExpression("(true).intersect(true.combine(true)) = true)"));

  Boolean true_proto = ParseFromString<Boolean>("value: true");
  Boolean false_proto = ParseFromString<Boolean>("value: false");
  EvaluationResult evaluation_result =
      Evaluate("(true | false).intersect(true | false)")
          .ValueOrDie();
  EXPECT_THAT(evaluation_result.GetMessages(),
              UnorderedElementsAreArray(
                  {EqualsProto(true_proto), EqualsProto(false_proto)}));
}

// TODO: Templatize tests to work with both STU3 and R4
TEST(FhirPathTest, TestDistinct) {
  EXPECT_THAT(Evaluate("{}.distinct()"), EvalsToEmpty());
  EXPECT_TRUE(EvaluateBoolExpression("true.distinct()"));
  EXPECT_TRUE(EvaluateBoolExpression("true.combine(true).distinct()"));

  Boolean true_proto = ParseFromString<Boolean>("value: true");
  Boolean false_proto = ParseFromString<Boolean>("value: false");
  EvaluationResult evaluation_result =
      Evaluate("(true | false).distinct()").ValueOrDie();
  EXPECT_THAT(evaluation_result.GetMessages(),
              UnorderedElementsAreArray(
                  {EqualsProto(true_proto), EqualsProto(false_proto)}));
}

TEST(FhirPathTest, TestIsDistinct) {
  EXPECT_TRUE(EvaluateBoolExpression("{}.isDistinct()"));
  EXPECT_TRUE(EvaluateBoolExpression("true.isDistinct()"));
  EXPECT_TRUE(EvaluateBoolExpression("(true | false).isDistinct()"));

  EXPECT_FALSE(EvaluateBoolExpression("true.combine(true).isDistinct()"));
}

TEST(FhirPathTest, TestIndexer) {
  EXPECT_TRUE(EvaluateBoolExpression("true[0] = true"));
  EXPECT_THAT(Evaluate("true[1]"), EvalsToEmpty());
  EXPECT_TRUE(EvaluateBoolExpression("false[0] = false"));
  EXPECT_THAT(Evaluate("false[1]"), EvalsToEmpty());

  EXPECT_FALSE(EvaluateBoolExpressionWithStatus("true['foo']").ok());
  EXPECT_FALSE(EvaluateBoolExpressionWithStatus("true[(1 | 2)]").ok());
}

TEST(FhirPathTest, TestContains) {
  EXPECT_TRUE(EvaluateBoolExpression("true contains true"));
  EXPECT_TRUE(EvaluateBoolExpression("(false | true) contains true"));

  EXPECT_FALSE(EvaluateBoolExpression("true contains false"));
  EXPECT_FALSE(EvaluateBoolExpression("(false | true) contains 1"));
  EXPECT_FALSE(EvaluateBoolExpression("{} contains true"));

  EXPECT_THAT(Evaluate("({} contains {})"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("(true contains {})"), EvalsToEmpty());

  EXPECT_FALSE(
      EvaluateBoolExpressionWithStatus("{} contains (true | false)").ok());
  EXPECT_FALSE(
      EvaluateBoolExpressionWithStatus("true contains (true | false)").ok());
}

TEST(FhirPathTest, TestIn) {
  EXPECT_TRUE(EvaluateBoolExpression("true in true"));
  EXPECT_TRUE(EvaluateBoolExpression("true in (false | true)"));

  EXPECT_FALSE(EvaluateBoolExpression("false in true"));
  EXPECT_FALSE(EvaluateBoolExpression("1 in (false | true)"));
  EXPECT_FALSE(EvaluateBoolExpression("true in {}"));

  EXPECT_THAT(Evaluate("({} in {})"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("({} in true)"), EvalsToEmpty());

  EXPECT_FALSE(EvaluateBoolExpressionWithStatus("(true | false) in {}").ok());
  EXPECT_FALSE(EvaluateBoolExpressionWithStatus("(true | false) in {}").ok());
}

TEST(FhirPathTest, TestImplies) {
  EXPECT_TRUE(EvaluateBoolExpression("(true implies true) = true"));
  EXPECT_TRUE(EvaluateBoolExpression("(true implies false) = false"));
  EXPECT_THAT(Evaluate("(true implies {})"), EvalsToEmpty());

  EXPECT_TRUE(EvaluateBoolExpression("(false implies true) = true"));
  EXPECT_TRUE(EvaluateBoolExpression("(false implies false) = true"));
  EXPECT_TRUE(EvaluateBoolExpression("(false implies {}) = true"));

  EXPECT_TRUE(EvaluateBoolExpression("({} implies true) = true"));
  EXPECT_THAT(Evaluate("({} implies false)"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("({} implies {})"), EvalsToEmpty());
}

TEST(FhirPathTest, TestWhere) {
  CodeableConcept observation = ParseFromString<CodeableConcept>(R"proto(
    coding {
      system { value: "foo" }
      code { value: "abc" }
    }
    coding {
      system { value: "bar" }
      code { value: "def" }
    }
    coding {
      system { value: "foo" }
      code { value: "ghi" }
    }
  )proto");

  Code code_abc = ParseFromString<Code>("value: 'abc'");
  Code code_ghi = ParseFromString<Code>("value: 'ghi'");
  EvaluationResult evaluation_result =
      CompiledExpression::Compile(CodeableConcept::descriptor(),
                                  "coding.where(system = 'foo').code")
          .ValueOrDie()
          .Evaluate(observation)
          .ValueOrDie();
  EXPECT_THAT(evaluation_result.GetMessages(),
              UnorderedElementsAreArray(
                  {EqualsProto(code_abc), EqualsProto(code_ghi)}));
}

TEST(FhirPathTest, TestWhereNoMatches) {
  EXPECT_THAT(Evaluate("('a' | 'b' | 'c').where(false)"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("{}.where(true)"), EvalsToEmpty());
}

TEST(FhirPathTest, TestWhereValidatesArguments) {
  EXPECT_FALSE(Evaluate("{}.where()").ok());
  EXPECT_TRUE(Evaluate("{}.where(true)").ok());
  EXPECT_FALSE(Evaluate("{}.where(true, false)").ok());
}

TEST(FhirPathTest, TestAll) {
  EXPECT_TRUE(EvaluateBoolExpression("{}.all(false)"));
  EXPECT_TRUE(EvaluateBoolExpression("(false).all(true)"));
  EXPECT_TRUE(EvaluateBoolExpression("(1 | 2 | 3).all($this < 4)"));
  EXPECT_FALSE(EvaluateBoolExpression("(1 | 2 | 3).all($this > 4)"));

  // Verify that all() fails when called with the wrong number of arguments.
  EXPECT_FALSE(Evaluate("{}.all()").ok());
  EXPECT_FALSE(Evaluate("{}.all(true, false)").ok());
}

TEST(FhirPathTest, TestAllReadsFieldFromDifferingTypes) {
  StructureDefinition structure_definition =
      ParseFromString<StructureDefinition>(R"proto(
        snapshot {
          element {}
        }
        differential {
          element {}
        }
      )proto");

  EvaluationResult evaluation_result =
      CompiledExpression::Compile(
          StructureDefinition::descriptor(),
          "(snapshot | differential).all(element.exists())")
          .ValueOrDie()
          .Evaluate(structure_definition)
          .ValueOrDie();
  EXPECT_TRUE(evaluation_result.GetBoolean().ValueOrDie());
}

// TODO: Templatize tests to work with both STU3 and R4
TEST(FhirPathTest, TestSelect) {
  EvaluationResult evaluation_result =
      Evaluate("(1 | 2 | 3).select(($this > 2) | $this)")
          .ValueOrDie();
  std::vector<const Message*> result = evaluation_result.GetMessages();

  Boolean true_proto = ParseFromString<Boolean>("value: true");
  Boolean false_proto = ParseFromString<Boolean>("value: false");
  Integer integer_1_proto = ParseFromString<Integer>("value: 1");
  Integer integer_2_proto = ParseFromString<Integer>("value: 2");
  Integer integer_3_proto = ParseFromString<Integer>("value: 3");

  ASSERT_THAT(
      result,
      UnorderedElementsAreArray({
        EqualsProto(true_proto),
        EqualsProto(false_proto),
        EqualsProto(false_proto),
        EqualsProto(integer_1_proto),
        EqualsProto(integer_2_proto),
        EqualsProto(integer_3_proto)
      }));
}

TEST(FhirPathTest, TestSelectEmptyResult) {
  EXPECT_THAT(Evaluate("{}.where(true)"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("(1 | 2 | 3).where(false)"), EvalsToEmpty());
}

TEST(FhirPathTest, TestSelectValidatesArguments) {
  EXPECT_FALSE(Evaluate("{}.select()").ok());
  EXPECT_TRUE(Evaluate("{}.select(true)").ok());
  EXPECT_FALSE(Evaluate("{}.select(true, false)").ok());
}

TEST(FhirPathTest, TestIif) {
  // 2 parameter invocations
  EXPECT_EQ(Evaluate("iif(true, 1)").ValueOrDie().GetInteger().ValueOrDie(), 1);
  EXPECT_THAT(Evaluate("iif(false, 1)"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("iif({}, 1)"), EvalsToEmpty());

  // 3 parameters invocations
  EXPECT_EQ(Evaluate("iif(true, 1, 2)").ValueOrDie().GetInteger().ValueOrDie(),
            1);
  EXPECT_EQ(Evaluate("iif(false, 1, 2)").ValueOrDie().GetInteger().ValueOrDie(),
            2);
  EXPECT_EQ(Evaluate("iif({}, 1, 2)").ValueOrDie().GetInteger().ValueOrDie(),
            2);

  EXPECT_THAT(Evaluate("{}.iif(true, false)"), EvalsToEmpty());
  EXPECT_FALSE(Evaluate("(1 | 2).iif(true, false)").ok());
}

TEST(FhirPathTest, TestIifValidatesArguments) {
  EXPECT_FALSE(Evaluate("{}.iif()").ok());
  EXPECT_FALSE(Evaluate("{}.iif(true)").ok());
  EXPECT_FALSE(
      Evaluate("{}.iif(true, false, true, false)").ok());
}

TEST(FhirPathTest, TestXor) {
  EXPECT_TRUE(EvaluateBoolExpression("(true xor true) = false"));
  EXPECT_TRUE(EvaluateBoolExpression("(true xor false) = true"));
  EXPECT_THAT(Evaluate("(true xor {})"), EvalsToEmpty());

  EXPECT_TRUE(EvaluateBoolExpression("(false xor true) = true"));
  EXPECT_TRUE(EvaluateBoolExpression("(false xor false) = false"));
  EXPECT_THAT(Evaluate("(false xor {})"), EvalsToEmpty());

  EXPECT_THAT(Evaluate("({} xor true)"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("({} xor false)"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("({} xor {})"), EvalsToEmpty());
}

TEST(FhirPathTest, TestOrShortCircuit) {
  auto expr =
      CompiledExpression::Compile(Quantity::descriptor(),
                                  "value.hasValue().not() or value < 100")
          .ValueOrDie();
  Quantity quantity;
  EvaluationResult result = expr.Evaluate(quantity).ValueOrDie();
  EXPECT_TRUE(result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestMultiOrShortCircuit) {
  auto expr =
      CompiledExpression::Compile(
          Period::descriptor(),
          "start.hasValue().not() or end.hasValue().not() or start <= end")
          .ValueOrDie();

  Period no_end_period = ParseFromString<Period>(R"proto(
    start: { value_us: 1556750000000 timezone: "America/Los_Angeles" }
  )proto");

  EvaluationResult result = expr.Evaluate(no_end_period).ValueOrDie();

  EXPECT_TRUE(result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestOrFalseWithEmptyReturnsEmpty) {
  auto expr = CompiledExpression::Compile(Quantity::descriptor(),
                                          "value.hasValue() or value < 100")
                  .ValueOrDie();
  Quantity quantity;
  EXPECT_THAT(expr.Evaluate(quantity), EvalsToEmpty());
}

TEST(FhirPathTest, TestOrOneIsTrue) {
  auto expr = CompiledExpression::Compile(
                  Encounter::descriptor(),
                  "period.start.exists() or period.end.exists()")
                  .ValueOrDie();

  Encounter test_encounter = ValidEncounter();

  EvaluationResult result = expr.Evaluate(test_encounter).ValueOrDie();

  EXPECT_TRUE(result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestOrNeitherAreTrue) {
  auto expr = CompiledExpression::Compile(
                  Encounter::descriptor(),
                  "hospitalization.exists() or location.exists()")
                  .ValueOrDie();

  Encounter test_encounter = ValidEncounter();

  EvaluationResult result = expr.Evaluate(test_encounter).ValueOrDie();

  EXPECT_FALSE(result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestAndShortCircuit) {
  auto expr = CompiledExpression::Compile(Quantity::descriptor(),
                                          "value.hasValue() and value < 100")
                  .ValueOrDie();
  Quantity quantity;
  EvaluationResult result = expr.Evaluate(quantity).ValueOrDie();
  EXPECT_FALSE(result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestAndTrueWithEmptyReturnsEmpty) {
  auto expr =
      CompiledExpression::Compile(Quantity::descriptor(),
                                  "value.hasValue().not() and value < 100")
          .ValueOrDie();
  Quantity quantity;
  EXPECT_THAT(expr.Evaluate(quantity), EvalsToEmpty());
}

TEST(FhirPathTest, TestAndOneIsTrue) {
  auto expr = CompiledExpression::Compile(
                  Encounter::descriptor(),
                  "period.start.exists() and period.end.exists()")
                  .ValueOrDie();

  Encounter test_encounter = ValidEncounter();

  EvaluationResult result = expr.Evaluate(test_encounter).ValueOrDie();

  EXPECT_FALSE(result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestAndBothAreTrue) {
  auto expr =
      CompiledExpression::Compile(Encounter::descriptor(),
                                  "period.start.exists() and status.exists()")
          .ValueOrDie();

  Encounter test_encounter = ValidEncounter();

  EvaluationResult result = expr.Evaluate(test_encounter).ValueOrDie();

  EXPECT_TRUE(result.GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, TestEmptyLiteral) {
  EXPECT_THAT(Evaluate("{}"), EvalsToEmpty());
}

TEST(FhirPathTest, TestBooleanLiteral) {
  EXPECT_TRUE(EvaluateBoolExpression("true"));
  EXPECT_FALSE(EvaluateBoolExpression("false"));
}

TEST(FhirPathTest, TestIntegerLiteral) {
  auto expr =
      CompiledExpression::Compile(Encounter::descriptor(), "42").ValueOrDie();

  Encounter test_encounter = ValidEncounter();
  EvaluationResult result = expr.Evaluate(test_encounter).ValueOrDie();
  EXPECT_EQ(42, result.GetInteger().ValueOrDie());

  // Ensure evaluation of an out-of-range literal fails.
  const char* overflow_value = "10000000000";
  Status bad_int_status =
      CompiledExpression::Compile(Encounter::descriptor(), overflow_value)
          .status();

  EXPECT_FALSE(bad_int_status.ok());
  // Failure message should contain the bad string.
  EXPECT_TRUE(bad_int_status.error_message().find(overflow_value) !=
              std::string::npos);
}

TEST(FhirPathTest, TestPolarityOperator) {
  EXPECT_TRUE(EvaluateBoolExpression("+1 = 1"));
  EXPECT_TRUE(EvaluateBoolExpression("-(+1) = -1"));
  EXPECT_TRUE(EvaluateBoolExpression("+(-1) = -1"));
  EXPECT_TRUE(EvaluateBoolExpression("-(-1) = 1"));

  EXPECT_TRUE(EvaluateBoolExpression("+1.2 = 1.2"));
  EXPECT_TRUE(EvaluateBoolExpression("-(+1.2) = -1.2"));
  EXPECT_TRUE(EvaluateBoolExpression("+(-1.2) = -1.2"));
  EXPECT_TRUE(EvaluateBoolExpression("-(-1.2) = 1.2"));

  EXPECT_THAT(Evaluate("+{}"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("-{}"), EvalsToEmpty());

  EXPECT_FALSE(Evaluate("+(1 | 2)").ok());
}

TEST(FhirPathTest, TestIntegerAddition) {
  EXPECT_TRUE(EvaluateBoolExpression("(2 + 3) = 5"));
  EXPECT_THAT(Evaluate("({} + 3)"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("(2 + {})"), EvalsToEmpty());
}

TEST(FhirPathTest, TestStringAddition) {
  EXPECT_TRUE(EvaluateBoolExpression("('foo' + 'bar') = 'foobar'"));
  EXPECT_THAT(Evaluate("({} + 'bar')"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("('foo' + {})"), EvalsToEmpty());
}

TEST(FhirPathTest, TestStringConcatenation) {
  EXPECT_EQ(EvaluateStringExpressionWithStatus("('foo' & 'bar')").ValueOrDie(),
            "foobar");
  EXPECT_EQ(EvaluateStringExpressionWithStatus("{} & 'bar'").ValueOrDie(),
            "bar");
  EXPECT_EQ(EvaluateStringExpressionWithStatus("'foo' & {}").ValueOrDie(),
            "foo");
  EXPECT_EQ(EvaluateStringExpressionWithStatus("{} & {}").ValueOrDie(), "");
}

TEST(FhirPathTest, TestEmptyComparisons) {
  EXPECT_THAT(Evaluate("{} = 42"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("42 = {}"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("{} = {}"), EvalsToEmpty());

  EXPECT_THAT(Evaluate("{} != 42"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("42 != {}"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("{} != {}"), EvalsToEmpty());

  EXPECT_THAT(Evaluate("{} < 42"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("42 < {}"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("{} < {}"), EvalsToEmpty());

  EXPECT_THAT(Evaluate("{} > 42"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("42 > {}"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("{} > {}"), EvalsToEmpty());

  EXPECT_THAT(Evaluate("{} >= 42"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("42 >= {}"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("{} >= {}"), EvalsToEmpty());

  EXPECT_THAT(Evaluate("{} <= 42"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("42 <= {}"), EvalsToEmpty());
  EXPECT_THAT(Evaluate("{} <= {}"), EvalsToEmpty());
}

TEST(FhirPathTest, TestIntegerComparisons) {
  EXPECT_TRUE(EvaluateBoolExpression("42 = 42"));
  EXPECT_FALSE(EvaluateBoolExpression("42 = 43"));

  EXPECT_TRUE(EvaluateBoolExpression("42 != 43"));
  EXPECT_FALSE(EvaluateBoolExpression("42 != 42"));

  EXPECT_TRUE(EvaluateBoolExpression("42 < 43"));
  EXPECT_FALSE(EvaluateBoolExpression("42 < 42"));

  EXPECT_TRUE(EvaluateBoolExpression("43 > 42"));
  EXPECT_FALSE(EvaluateBoolExpression("42 > 42"));

  EXPECT_TRUE(EvaluateBoolExpression("42 >= 42"));
  EXPECT_TRUE(EvaluateBoolExpression("43 >= 42"));
  EXPECT_FALSE(EvaluateBoolExpression("42 >= 43"));

  EXPECT_TRUE(EvaluateBoolExpression("42 <= 42"));
  EXPECT_TRUE(EvaluateBoolExpression("42 <= 43"));
  EXPECT_FALSE(EvaluateBoolExpression("43 <= 42"));
}

TEST(FhirPathTest, TestIntegerLikeComparison) {
  Parameters parameters =
      ParseFromString<Parameters>(R"proto(
        parameter {value {integer {value: -1}}}
        parameter {value {integer {value: 0}}}
        parameter {value {integer {value: 1}}}
        parameter {value {unsigned_int {value: 0}}}
      )proto");

  // lhs = -1 (signed), rhs = 0 (unsigned)
  EXPECT_THAT(Evaluate(parameters, "parameter[0].value < parameter[3].value"),
              EvalsToTrue());
  EXPECT_THAT(Evaluate(parameters, "parameter[0].value <= parameter[3].value"),
              EvalsToTrue());
  EXPECT_THAT(Evaluate(parameters, "parameter[0].value >= parameter[3].value"),
              EvalsToFalse());
  EXPECT_THAT(Evaluate(parameters, "parameter[0].value > parameter[3].value"),
              EvalsToFalse());

  // lhs = 0 (signed), rhs = 0 (unsigned)
  EXPECT_THAT(Evaluate(parameters, "parameter[1].value < parameter[3].value"),
              EvalsToFalse());
  EXPECT_THAT(Evaluate(parameters, "parameter[1].value <= parameter[3].value"),
              EvalsToTrue());
  EXPECT_THAT(Evaluate(parameters, "parameter[1].value >= parameter[3].value"),
              EvalsToTrue());
  EXPECT_THAT(Evaluate(parameters, "parameter[1].value > parameter[3].value"),
              EvalsToFalse());

  // lhs = 1 (signed), rhs = 0 (unsigned)
  EXPECT_THAT(Evaluate(parameters, "parameter[2].value < parameter[3].value"),
              EvalsToFalse());
  EXPECT_THAT(Evaluate(parameters, "parameter[2].value <= parameter[3].value"),
              EvalsToFalse());
  EXPECT_THAT(Evaluate(parameters, "parameter[2].value >= parameter[3].value"),
              EvalsToTrue());
  EXPECT_THAT(Evaluate(parameters, "parameter[2].value > parameter[3].value"),
              EvalsToTrue());
}

TEST(FhirPathTest, TestDecimalLiteral) {
  auto expr =
      CompiledExpression::Compile(Encounter::descriptor(), "1.25").ValueOrDie();

  Encounter test_encounter = ValidEncounter();
  EvaluationResult result = expr.Evaluate(test_encounter).ValueOrDie();

  EXPECT_EQ("1.25", result.GetDecimal().ValueOrDie());
}

TEST(FhirPathTest, TestDecimalComparisons) {
  EXPECT_TRUE(EvaluateBoolExpression("1.25 = 1.25"));
  EXPECT_FALSE(EvaluateBoolExpression("1.25 = 1.3"));

  EXPECT_TRUE(EvaluateBoolExpression("1.25 != 1.26"));
  EXPECT_FALSE(EvaluateBoolExpression("1.25 != 1.25"));

  EXPECT_TRUE(EvaluateBoolExpression("1.25 < 1.26"));
  EXPECT_TRUE(EvaluateBoolExpression("1 < 1.26"));
  EXPECT_FALSE(EvaluateBoolExpression("1.25 < 1.25"));

  EXPECT_TRUE(EvaluateBoolExpression("1.26 > 1.25"));
  EXPECT_TRUE(EvaluateBoolExpression("1.26 > 1"));
  EXPECT_FALSE(EvaluateBoolExpression("1.25 > 1.25"));

  EXPECT_TRUE(EvaluateBoolExpression("1.25 >= 1.25"));
  EXPECT_TRUE(EvaluateBoolExpression("1.25 >= 1"));
  EXPECT_TRUE(EvaluateBoolExpression("1.26 >= 1.25"));
  EXPECT_FALSE(EvaluateBoolExpression("1.25 >= 1.26"));

  EXPECT_TRUE(EvaluateBoolExpression("1.25 <= 1.25"));
  EXPECT_TRUE(EvaluateBoolExpression("1.25 <= 1.26"));
  EXPECT_FALSE(EvaluateBoolExpression("1.26 <= 1.25"));
  EXPECT_FALSE(EvaluateBoolExpression("1.26 <= 1"));
}

TEST(FhirPathTest, TestStringLiteral) {
  auto expr = CompiledExpression::Compile(Encounter::descriptor(), "'foo'")
                  .ValueOrDie();

  Encounter test_encounter = ValidEncounter();
  EvaluationResult result = expr.Evaluate(test_encounter).ValueOrDie();
  EXPECT_EQ("foo", result.GetString().ValueOrDie());
}

TEST(FhirPathTest, TestStringLiteralEscaping) {
  EXPECT_EQ("\\", EvaluateStringExpressionWithStatus("'\\\\'").ValueOrDie());
  EXPECT_EQ("\f", EvaluateStringExpressionWithStatus("'\\f'").ValueOrDie());
  EXPECT_EQ("\n", EvaluateStringExpressionWithStatus("'\\n'").ValueOrDie());
  EXPECT_EQ("\r", EvaluateStringExpressionWithStatus("'\\r'").ValueOrDie());
  EXPECT_EQ("\t", EvaluateStringExpressionWithStatus("'\\t'").ValueOrDie());
  EXPECT_EQ("\"", EvaluateStringExpressionWithStatus("'\\\"'").ValueOrDie());
  EXPECT_EQ("'", EvaluateStringExpressionWithStatus("'\\\''").ValueOrDie());
  EXPECT_EQ("\t", EvaluateStringExpressionWithStatus("'\\t'").ValueOrDie());
  EXPECT_EQ(" ", EvaluateStringExpressionWithStatus("'\\u0020'").ValueOrDie());

  // Disallowed escape sequences
  EXPECT_FALSE(EvaluateStringExpressionWithStatus("'\\x20'").ok());
  EXPECT_FALSE(EvaluateStringExpressionWithStatus("'\\123'").ok());
  EXPECT_FALSE(EvaluateStringExpressionWithStatus("'\\x20'").ok());
  EXPECT_FALSE(EvaluateStringExpressionWithStatus("'\\x00000020'").ok());
}

TEST(FhirPathTest, TestStringComparisons) {
  EXPECT_TRUE(EvaluateBoolExpression("'foo' = 'foo'"));
  EXPECT_FALSE(EvaluateBoolExpression("'foo' = 'bar'"));

  EXPECT_TRUE(EvaluateBoolExpression("'foo' != 'bar'"));
  EXPECT_FALSE(EvaluateBoolExpression("'foo' != 'foo'"));

  EXPECT_TRUE(EvaluateBoolExpression("'bar' < 'foo'"));
  EXPECT_FALSE(EvaluateBoolExpression("'foo' < 'foo'"));

  EXPECT_TRUE(EvaluateBoolExpression("'foo' > 'bar'"));
  EXPECT_FALSE(EvaluateBoolExpression("'foo' > 'foo'"));

  EXPECT_TRUE(EvaluateBoolExpression("'foo' >= 'foo'"));
  EXPECT_TRUE(EvaluateBoolExpression("'foo' >= 'bar'"));
  EXPECT_FALSE(EvaluateBoolExpression("'bar' >= 'foo'"));

  EXPECT_TRUE(EvaluateBoolExpression("'foo' <= 'foo'"));
  EXPECT_TRUE(EvaluateBoolExpression("'bar' <= 'foo'"));
  EXPECT_FALSE(EvaluateBoolExpression("'foo' <= 'bar'"));
}

// TODO: Templatize tests to work with both STU3 and R4
TEST(FhirPathTest, ConstraintViolation) {
  Observation observation = ValidObservation();

  // If a range is present it must have a high or low value,
  // so ensure the constraint fails if it doesn't.
  observation.add_reference_range();

  MessageValidator validator;

  auto callback = [&observation](const Message& bad_message,
                                 const FieldDescriptor* field,
                                 const std::string& constraint) {
    // Ensure the expected bad sub-message is passed to the callback.
    EXPECT_EQ(observation.reference_range(0).GetDescriptor()->full_name(),
              bad_message.GetDescriptor()->full_name());

    // Ensure the expected constraint failed.
    EXPECT_EQ("low.exists() or high.exists() or text.exists()", constraint);

    return false;
  };

  std::string err_message =
      absl::StrCat("fhirpath-constraint-violation-ReferenceRange: ",
                   "\"low.exists() or high.exists() or text.exists()\"");
  EXPECT_EQ(validator.Validate(observation, callback),
            ::tensorflow::errors::FailedPrecondition(err_message));
}

TEST(FhirPathTest, ConstraintSatisfied) {
  Observation observation = ValidObservation();

  // Ensure constraint succeeds with a value in the reference range
  // as required by FHIR.
  auto ref_range = observation.add_reference_range();

  auto value = new Decimal();
  value->set_allocated_value(new std::string("123.45"));

  auto high = new SimpleQuantity();
  high->set_allocated_value(value);

  ref_range->set_allocated_high(high);

  MessageValidator validator;

  EXPECT_TRUE(validator.Validate(observation).ok());
}

TEST(FhirPathTest, NestedConstraintViolated) {
  ValueSet value_set = ValidValueSet();

  auto expansion = new ValueSet::Expansion;

  // Add empty contains structure to violate FHIR constraint.
  expansion->add_contains();
  value_set.mutable_name()->set_value("Placeholder");
  value_set.set_allocated_expansion(expansion);

  MessageValidator validator;

  std::string err_message =
      absl::StrCat("fhirpath-constraint-violation-Contains: ",
                   "\"code.exists() or display.exists()\"");

  EXPECT_EQ(validator.Validate(value_set),
            ::tensorflow::errors::FailedPrecondition(err_message));
}

TEST(FhirPathTest, NestedConstraintSatisfied) {
  ValueSet value_set = ValidValueSet();
  value_set.mutable_name()->set_value("Placeholder");

  auto expansion = new ValueSet::Expansion;
  auto contains = expansion->add_contains();

  // Contains struct has value to satisfy FHIR constraint.
  auto proto_string = new String();
  proto_string->set_value("Placeholder value");
  contains->set_allocated_display(proto_string);

  auto proto_boolean = new Boolean();
  proto_boolean->set_value(true);
  contains->set_allocated_abstract(proto_boolean);

  value_set.set_allocated_expansion(expansion);

  MessageValidator validator;

  EXPECT_TRUE(validator.Validate(value_set).ok());
}

// TODO: Templatize tests to work with both STU3 and R4
TEST(FhirPathTest, TimeComparison) {
  auto start_before_end =
      CompiledExpression::Compile(Period::descriptor(), "start <= end")
          .ValueOrDie();

  Period start_before_end_period = ParseFromString<Period>(R"proto(
    start: { value_us: 1556750000000000 timezone: "America/Los_Angeles" }
    end: { value_us: 1556750153000000 timezone: "America/Los_Angeles" }
  )proto");
  EvaluationResult start_before_end_result =
      start_before_end.Evaluate(start_before_end_period).ValueOrDie();
  EXPECT_TRUE(start_before_end_result.GetBoolean().ValueOrDie());

  Period end_before_start_period = ParseFromString<Period>(R"proto(
    start: { value_us: 1556750153000000 timezone: "America/Los_Angeles" }
    end: { value_us: 1556750000000000 timezone: "America/Los_Angeles" }
  )proto");
  EvaluationResult end_before_start_result =
      start_before_end.Evaluate(end_before_start_period).ValueOrDie();
  EXPECT_FALSE(end_before_start_result.GetBoolean().ValueOrDie());
}

// TODO: Templatize tests to work with both STU3 and R4
TEST(FhirPathTest, TimeCompareDifferentPrecision) {
  absl::TimeZone zone;
  absl::LoadTimeZone("America/Los_Angeles", &zone);
  auto start_before_end =
      CompiledExpression::Compile(Period::descriptor(), "start <= end")
          .ValueOrDie();

  // Ensure comparison returns false on fine-grained checks but true
  // on corresponding coarse-grained checks.
  EXPECT_FALSE(EvaluateOnPeriod(
      start_before_end,
      ToDateTime(absl::CivilSecond(2019, 5, 2, 22, 33, 53), zone,
                 DateTime::SECOND),
      ToDateTime(absl::CivilDay(2019, 5, 2), zone, DateTime::SECOND)));

  EXPECT_TRUE(EvaluateOnPeriod(
      start_before_end,
      ToDateTime(absl::CivilSecond(2019, 5, 2, 22, 33, 53), zone,
                 DateTime::SECOND),
      ToDateTime(absl::CivilDay(2019, 5, 2), zone, DateTime::DAY)));

  EXPECT_FALSE(EvaluateOnPeriod(
      start_before_end,
      ToDateTime(absl::CivilSecond(2019, 5, 2, 22, 33, 53), zone,
                 DateTime::SECOND),
      ToDateTime(absl::CivilDay(2019, 5, 1), zone, DateTime::DAY)));

  EXPECT_TRUE(EvaluateOnPeriod(
      start_before_end,
      ToDateTime(absl::CivilSecond(2019, 5, 2, 22, 33, 53), zone,
                 DateTime::SECOND),
      ToDateTime(absl::CivilDay(2019, 5, 1), zone, DateTime::MONTH)));

  EXPECT_FALSE(EvaluateOnPeriod(
      start_before_end,
      ToDateTime(absl::CivilSecond(2019, 5, 2, 22, 33, 53), zone,
                 DateTime::SECOND),
      ToDateTime(absl::CivilDay(2019, 1, 1), zone, DateTime::MONTH)));

  EXPECT_TRUE(EvaluateOnPeriod(
      start_before_end,
      ToDateTime(absl::CivilSecond(2019, 5, 2, 22, 33, 53), zone,
                 DateTime::SECOND),
      ToDateTime(absl::CivilDay(2019, 1, 1), zone, DateTime::YEAR)));

  // Test edge case for very high precision comparisons.
  DateTime start_micros;
  start_micros.set_value_us(1556750000000011);
  start_micros.set_timezone("America/Los_Angeles");
  start_micros.set_precision(DateTime::MICROSECOND);

  DateTime end_micros = start_micros;
  EXPECT_TRUE(EvaluateOnPeriod(start_before_end, start_micros, end_micros));

  end_micros.set_value_us(end_micros.value_us() - 1);
  EXPECT_FALSE(EvaluateOnPeriod(start_before_end, start_micros, end_micros));
}

TEST(FhirPathTest, SimpleQuantityComparisons) {
  MedicationKnowledge_Kinetics kinetics =
      ParseFromString<MedicationKnowledge_Kinetics>(R"proto(
        area_under_curve {
          value { value: "1.1"}
          system { value: "http://valuesystem.example.org/foo" }
          code { value: "bar" }
        }
        area_under_curve {
          value { value: "1.2"}
          system { value: "http://valuesystem.example.org/foo" }
          code { value: "bar" }
        }
        area_under_curve {
          value { value: "1.1"}
          system { value: "http://valuesystem.example.org/foo" }
          code { value: "different" }
        }
        area_under_curve {
          value { value: "1.1"}
          system { value: "http://valuesystem.example.org/different" }
          code { value: "bar" }
        }
      )proto");

  EXPECT_THAT(Evaluate(kinetics, "areaUnderCurve[0] < areaUnderCurve[0]"),
              EvalsToFalse());
  EXPECT_THAT(Evaluate(kinetics, "areaUnderCurve[0] <= areaUnderCurve[0]"),
              EvalsToTrue());
  EXPECT_THAT(Evaluate(kinetics, "areaUnderCurve[0] >= areaUnderCurve[0]"),
              EvalsToTrue());
  EXPECT_THAT(Evaluate(kinetics, "areaUnderCurve[0] > areaUnderCurve[0]"),
              EvalsToFalse());

  EXPECT_THAT(Evaluate(kinetics, "areaUnderCurve[1] < areaUnderCurve[0]"),
              EvalsToFalse());
  EXPECT_THAT(Evaluate(kinetics, "areaUnderCurve[1] <= areaUnderCurve[0]"),
              EvalsToFalse());
  EXPECT_THAT(Evaluate(kinetics, "areaUnderCurve[1] >= areaUnderCurve[0]"),
              EvalsToTrue());
  EXPECT_THAT(Evaluate(kinetics, "areaUnderCurve[1] > areaUnderCurve[0]"),
              EvalsToTrue());

  EXPECT_THAT(Evaluate(kinetics, "areaUnderCurve[0] < areaUnderCurve[1]"),
              EvalsToTrue());
  EXPECT_THAT(Evaluate(kinetics, "areaUnderCurve[0] <= areaUnderCurve[1]"),
              EvalsToTrue());
  EXPECT_THAT(Evaluate(kinetics, "areaUnderCurve[0] >= areaUnderCurve[1]"),
              EvalsToFalse());
  EXPECT_THAT(Evaluate(kinetics, "areaUnderCurve[0] > areaUnderCurve[1]"),
              EvalsToFalse());

  // Different quantity codes
  EXPECT_FALSE(Evaluate(
                   kinetics, "areaUnderCurve[0] > areaUnderCurve[2]")
                   .ok());

  // Different quantity systems
  EXPECT_FALSE(Evaluate(
                   kinetics, "areaUnderCurve[0] > areaUnderCurve[3]")
                   .ok());
}

TEST(FhirPathTest, TestCompareEnumToString) {
  auto encounter = ValidEncounter();
  auto is_triaged =
      CompiledExpression::Compile(Encounter::descriptor(), "status = 'triaged'")
          .ValueOrDie();

  EXPECT_TRUE(
      is_triaged.Evaluate(encounter).ValueOrDie().GetBoolean().ValueOrDie());
  encounter.mutable_status()->set_value(
      r4::core::EncounterStatusCode::FINISHED);
  EXPECT_FALSE(
      is_triaged.Evaluate(encounter).ValueOrDie().GetBoolean().ValueOrDie());
}

TEST(FhirPathTest, MessageLevelConstraint) {
  Period period = ParseFromString<Period>(R"proto(
    start: { value_us: 1556750000000000 timezone: "America/Los_Angeles" }
    end: { value_us: 1556750153000000 timezone: "America/Los_Angeles" }
  )proto");

  MessageValidator validator;
  EXPECT_TRUE(validator.Validate(period).ok());
}

// TODO: Templatize tests to work with both STU3 and R4
TEST(FhirPathTest, MessageLevelConstraintViolated) {
  Period end_before_start_period = ParseFromString<Period>(R"proto(
    start: { value_us: 1556750153000000 timezone: "America/Los_Angeles" }
    end: { value_us: 1556750000000000 timezone: "America/Los_Angeles" }
  )proto");

  MessageValidator validator;
  EXPECT_FALSE(validator.Validate(end_before_start_period).ok());
}

TEST(FhirPathTest, NestedMessageLevelConstraint) {
  auto start_with_no_end_encounter = ParseFromString<Encounter>(R"proto(
    status { value: TRIAGED }
    id { value: "123" }
    period {
      start: { value_us: 1556750153000000 timezone: "America/Los_Angeles" }
    }
  )proto");

  MessageValidator validator;
  EXPECT_TRUE(validator.Validate(start_with_no_end_encounter).ok());
}

TEST(FhirPathTest, NestedMessageLevelConstraintViolated) {
  auto end_before_start_encounter = ParseFromString<Encounter>(R"proto(
    status { value: TRIAGED }
    id { value: "123" }
    period {
      start: { value_us: 1556750153000000 timezone: "America/Los_Angeles" }
      end: { value_us: 1556750000000000 timezone: "America/Los_Angeles" }
    }
  )proto");

  MessageValidator validator;
  EXPECT_FALSE(validator.Validate(end_before_start_encounter).ok());
}

TEST(FhirPathTest, ProfiledEmptyExtension) {
  USCorePatientProfile patient = ValidUsCorePatient();
  MessageValidator validator;
  EXPECT_TRUE(validator.Validate(patient).ok());
}

TEST(FhirPathTest, ProfiledWithExtensions) {
  USCorePatientProfile patient = ValidUsCorePatient();
  auto race = new r4::uscore::PatientUSCoreRaceExtension();

  r4::uscore::PatientUSCoreRaceExtension::OmbCategoryCoding* coding =
      race->add_omb_category();
  coding->mutable_code()->set_value(
      r4::uscore::OmbRaceCategoriesValueSet::AMERICAN_INDIAN_OR_ALASKA_NATIVE);
  patient.set_allocated_race(race);

  patient.mutable_birthsex()->set_value(BirthSexValueSet::M);

  MessageValidator validator;
  EXPECT_TRUE(validator.Validate(patient).ok());
}

}  // namespace

}  // namespace fhir_path
}  // namespace fhir
}  // namespace google
