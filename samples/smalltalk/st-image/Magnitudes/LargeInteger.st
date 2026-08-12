LargeInteger := Integer [

    <shape: largeInteger>
    <abstract: true>

    = anInteger [
        (anInteger isKindOf: Integer) ifTrue: [
            ^(self largeCompare: anInteger) = 0
        ].
        (anInteger isKindOf: Fraction) ifTrue: [
            ^anInteger equalsInteger: self
        ].
        ^false
    ]

    < anInteger [
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

    + aNumber [
        (aNumber isKindOf: Integer) ifTrue: [
            ^self largeOp: 1 with: aNumber
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            ^aNumber addInteger: self
        ].
        (aNumber isKindOf: Float) ifTrue: [
            ^self asFloat + aNumber
        ].
        ^InvalidArgumentError signal
    ]

    - aNumber [
        (aNumber isKindOf: Integer) ifTrue: [
            ^self largeOp: 2 with: aNumber
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            ^aNumber subtractFromInteger: self
        ].
        (aNumber isKindOf: Float) ifTrue: [
            ^self asFloat - aNumber
        ].
        ^InvalidArgumentError signal
    ]

    * aNumber [
        (aNumber isKindOf: Integer) ifTrue: [
            ^self largeOp: 3 with: aNumber
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            ^aNumber multiplyInteger: self
        ].
        (aNumber isKindOf: Float) ifTrue: [
            ^self asFloat * aNumber
        ].
        ^InvalidArgumentError signal
    ]

    // anInteger [
        anInteger = 0 ifTrue: [
            ^ZeroDivide signal
        ].
        ^self largeOp: 6 with: anInteger
    ]

    \\ anInteger [
        anInteger = 0 ifTrue: [
            ^ZeroDivide signal
        ].
        ^self largeOp: 7 with: anInteger
    ]

    negated [
        ^0 largeOp: 2 with: self
    ]

    bitShift: aCount [
        ^self largeShift: aCount
    ]

    asFloat [
        <primitive: LargeIntAsFloatPrimitive>
        ^InvalidArgumentError signal
    ]
]
