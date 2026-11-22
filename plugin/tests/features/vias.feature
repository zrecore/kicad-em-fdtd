Feature: Vias are exported with the copper layers they join
  A blind or micro via joins only some layers; the solver must not treat it as a through via.

  Scenario: A through via carries no layer span
    Given a via from F.Cu to B.Cu
    When the vias are read
    Then the via has no layer span

  Scenario: A micro via between two layers carries its span
    Given a via from In4.Cu to B.Cu
    When the vias are read
    Then the via spans from In4.Cu to B.Cu

  Scenario: A board API that gives no drill layers still exports the via
    Given a via whose drill layers cannot be read
    When the vias are read
    Then the via has no layer span
