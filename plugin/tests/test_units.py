import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin import units

scenarios("features/units.feature")


@pytest.fixture
def context():
    return {}


@given(parsers.parse("a length of {value:d} nanometres reported by KiCAD"))
def _given_nm(context, value):
    context["convert"] = lambda: units.nm_to_m(value)


@given(parsers.parse("a length of {value:g} millimetres from a datasheet"))
def _given_mm(context, value):
    context["convert"] = lambda: units.mm_to_m(value)


@when("it is converted to metres")
def _when_convert(context):
    context["result"] = context["convert"]()


@then(parsers.parse("the result is {expected:g} metres"))
def _then_result(context, expected):
    assert context["result"] == pytest.approx(expected)
