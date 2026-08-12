SmallInteger := Integer [

    class minVal [
        ^-1152921504606846976
    ]

    class maxVal [
        ^1152921504606846975
    ]

    = anInteger [
        <primitive: IntEqualsPrimitive>
        (anInteger isKindOf: Integer) ifTrue: [
            ^(self largeCompare: anInteger) = 0
        ].
        (anInteger isKindOf: Fraction) ifTrue: [
            ^anInteger equalsInteger: self
        ].
        ^false
    ]

    < anInteger [
        <primitive: IntLessThanPrimitive>
        (anInteger isKindOf: Integer) ifTrue: [
            ^(self largeCompare: anInteger) < 0
        ].
        (anInteger isKindOf: Fraction) ifTrue: [
            ^anInteger greaterThanInteger: self
        ].
        (anInteger isKindOf: Float) ifTrue: [
            ^self asFloat < anInteger
        ].
        ^InvalidArgumentError signal
    ]

    > anInteger [
        <primitive: IntGreaterThanPrimitive>
        ^anInteger < self
    ]

    <= anInteger [
        <primitive: IntLessEqualsPrimitive>
        ^(anInteger < self) not
    ]

    >= anInteger [
        <primitive: IntGreaterEqualsPrimitive>
        ^(self < anInteger) not
    ]

    + anInteger [
        <primitive: IntAddPrimitive>
        (anInteger isKindOf: Integer) ifTrue: [
            ^self largeOp: 1 with: anInteger
        ].
        (anInteger isKindOf: Fraction) ifTrue: [
            ^anInteger addInteger: self
        ].
        (anInteger isKindOf: Float) ifTrue: [
            ^self asFloat + anInteger
        ].
        ^InvalidArgumentError signal
    ]

    - anInteger [
        <primitive: IntSubPrimitive>
        (anInteger isKindOf: Integer) ifTrue: [
            ^self largeOp: 2 with: anInteger
        ].
        (anInteger isKindOf: Fraction) ifTrue: [
            ^anInteger subtractFromInteger: self
        ].
        (anInteger isKindOf: Float) ifTrue: [
            ^self asFloat - anInteger
        ].
        ^InvalidArgumentError signal
    ]

    * anInteger [
        <primitive: IntMulPrimitive>
        (anInteger isKindOf: Integer) ifTrue: [
            ^self largeOp: 3 with: anInteger
        ].
        (anInteger isKindOf: Fraction) ifTrue: [
            ^anInteger multiplyInteger: self
        ].
        (anInteger isKindOf: Float) ifTrue: [
            ^self asFloat * anInteger
        ].
        ^InvalidArgumentError signal
    ]

    // anInteger [
        <primitive: IntFloorDivPrimitive>
        anInteger = 0 ifTrue: [
            ^ZeroDivide signal
        ].
        (anInteger isKindOf: Integer) ifTrue: [
            ^self largeOp: 6 with: anInteger
        ].
        ^InvalidArgumentError signal
    ]

    \\ anInteger [
        <primitive: IntModPrimitive>
        anInteger = 0 ifTrue: [
            ^ZeroDivide signal
        ].
        (anInteger isKindOf: Integer) ifTrue: [
            ^self largeOp: 7 with: anInteger
        ].
        ^InvalidArgumentError signal
    ]

    negated [
        <primitive: IntNegPrimitive>
        ^0 largeOp: 2 with: self
    ]

    bitAnd: anInteger [
        <primitive: IntAndPrimitive>
        ^InvalidArgumentError signal
    ]

    bitOr: anInteger [
        <primitive: IntOrPrimitive>
        ^InvalidArgumentError signal
    ]

    bitXor: anInteger [
        <primitive: IntXorPrimitive>
        ^InvalidArgumentError signal
    ]

    bitShift: aCount [
        <primitive: IntShiftPrimitive>
        (aCount isKindOf: Integer) ifTrue: [
            ^self largeShift: aCount
        ].
        ^InvalidArgumentError signal
    ]

    asFloat [
        <primitive: IntAsFloatPrimitive>
        ^InvalidArgumentError signal
    ]
]
