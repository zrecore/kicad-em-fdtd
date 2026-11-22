Feature: The board outline and its cutouts are exported as drawn on Edge.Cuts
  A rounded corner, a routed slot or an opening changes the board, so none of them is replaced by a rectangle.

  Scenario: Four lines make a rectangular board
    Given Edge.Cuts with a 20 mm by 10 mm rectangle drawn as four lines
    When the outline is read
    Then the board is a ring of 4 points with an area of 200 square mm
    And there are no cutouts

  Scenario: A rounded corner is followed along its arc
    Given Edge.Cuts with a 20 mm by 10 mm rectangle whose top right corner is an arc of radius 3 mm
    When the outline is read
    Then the board area is less than 200 square mm by about 1.93 square mm
    And there are no cutouts

  Scenario: A circle or a slot inside the board is a cutout
    Given Edge.Cuts with a 20 mm by 10 mm rectangle and a circle of radius 2 mm inside it
    When the outline is read
    Then the board is the rectangle
    And there is 1 cutout of area about 12.5 square mm

  Scenario: A loop that is not closed is reported, not silently dropped
    Given Edge.Cuts with a 20 mm by 10 mm rectangle and a stray line
    When the outline is read
    Then the notes mention 1 piece that does not close

  Scenario: A board inside a panel is separated from its frame at the breakaway tabs
    Given a 60 mm by 40 mm panel with a 40 mm by 20 mm board held by four corner tabs across 1.6 mm slots
    When the panel frame is excluded around the board centre
    Then the board is about 40 mm by 20 mm and holds 0 slots
    And the notes mention the panel frame

  Scenario: A board with one isolation slot is not taken for a panel
    Given Edge.Cuts with a 20 mm by 10 mm rectangle and a circle of radius 2 mm inside it
    When the panel frame is excluded around the board centre
    Then no panel is found
