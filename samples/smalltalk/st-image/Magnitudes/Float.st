Float := Number [
    <abstract: true>

    = aNumber [
        <primitive: FloatEqualsPrimitive>
    ]

    < aNumber [
        <primitive: FloatLessThanPrimitive>
        (aNumber isKindOf: Integer) ifTrue: [
            ^self < aNumber asFloat
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            ^self < aNumber asFloat
        ].
        ^InvalidArgumentError signal
    ]

    > aNumber [
        <primitive: FloatGreaterThanPrimitive>
        (aNumber isKindOf: Integer) ifTrue: [
            ^self > aNumber asFloat
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            ^self > aNumber asFloat
        ].
        ^InvalidArgumentError signal
    ]

    <= aNumber [
        <primitive: FloatLessEqualsPrimitive>
        (aNumber isKindOf: Integer) ifTrue: [
            ^self <= aNumber asFloat
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            ^self <= aNumber asFloat
        ].
        ^InvalidArgumentError signal
    ]

    >= aNumber [
        <primitive: FloatGreaterEqualsPrimitive>
        (aNumber isKindOf: Integer) ifTrue: [
            ^self >= aNumber asFloat
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            ^self >= aNumber asFloat
        ].
        ^InvalidArgumentError signal
    ]

    + aNumber [
        <primitive: FloatAddPrimitive>
        (aNumber isKindOf: Integer) ifTrue: [
            ^self + aNumber asFloat
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            ^self + aNumber asFloat
        ].
        ^InvalidArgumentError signal
    ]

    - aNumber [
        <primitive: FloatSubPrimitive>
        (aNumber isKindOf: Integer) ifTrue: [
            ^self - aNumber asFloat
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            ^self - aNumber asFloat
        ].
        ^InvalidArgumentError signal
    ]

    * aNumber [
        <primitive: FloatMulPrimitive>
        (aNumber isKindOf: Integer) ifTrue: [
            ^self * aNumber asFloat
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            ^self * aNumber asFloat
        ].
        ^InvalidArgumentError signal
    ]

    / aNumber [
        <primitive: FloatDivPrimitive>
        (aNumber isKindOf: Integer) ifTrue: [
            ^self / aNumber asFloat
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            ^self / aNumber asFloat
        ].
        ^InvalidArgumentError signal
    ]

    negated [
        <primitive: FloatNegPrimitive>
        ^Error signal
    ]

    truncated [
        <primitive: FloatTruncatedPrimitive>
        ^ArithmeticError signal
    ]

    floor [
        <primitive: FloatFloorPrimitive>
        ^ArithmeticError signal
    ]

    ceiling [
        <primitive: FloatCeilingPrimitive>
        ^ArithmeticError signal
    ]

    rounded [
        <primitive: FloatRoundedPrimitive>
        ^ArithmeticError signal
    ]

    hash [
        <primitive: FloatHashPrimitive>
    ]

    asFloat [
        ^self
    ]
]
