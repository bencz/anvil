Integer := Number [

    <abstract: true>

    hash [
        <primitive: IntegerHashPrimitive>
    ]

    asString [
        | digits magnitude count value result position |
        digits := '0123456789'.
        magnitude := self abs.
        count := 1.
        value := magnitude.

        [value >= 10] whileTrue: [
            count := count + 1.
            value := value // 10
        ].

        self < 0 ifTrue: [count := count + 1].
        result := String new: count.
        position := count.

        [position > 0] whileTrue: [
            result at: position put: (digits at: (magnitude \\ 10) + 1).
            magnitude := magnitude // 10.
            position := position - 1
        ].

        self < 0 ifTrue: [result at: 1 put: $-].
        ^result
    ]

    / aNumber [
        (aNumber isKindOf: Integer) ifTrue: [
            ^Fraction numerator: self denominator: aNumber
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            ^aNumber divideIntegerInto: self
        ].
        (aNumber isKindOf: Float) ifTrue: [
            ^self asFloat / aNumber
        ].
        ^InvalidArgumentError signal
    ]

    // anInteger [
        ^self subclassResponsibility
    ]

    \\ anInteger [
        ^self subclassResponsibility
    ]

    bitAnd: anInteger [
        ^self subclassResponsibility
    ]

    bitOr: anInteger [
        ^self subclassResponsibility
    ]

    bitXor: anInteger [
        ^self subclassResponsibility
    ]

    bitShift: aCount [
        ^self subclassResponsibility
    ]

    largeOp: opcode with: anInteger [
        <primitive: LargeIntBinaryPrimitive>
        ^InvalidArgumentError signal
    ]

    largeCompare: anInteger [
        <primitive: LargeIntComparePrimitive>
        ^InvalidArgumentError signal
    ]

    largeShift: aCount [
        <primitive: LargeIntShiftPrimitive>
        ^InvalidArgumentError signal
    ]

    quo: anInteger [
        | quotient |
        (anInteger isKindOf: Integer) ifFalse: [
            ^InvalidArgumentError signal
        ].
        anInteger = 0 ifTrue: [
            ^ZeroDivide signal
        ].
        quotient := self abs // anInteger abs.
        (self < 0) = (anInteger < 0) ifTrue: [
            ^quotient
        ].
        ^quotient negated
    ]

    rem: anInteger [
        (anInteger isKindOf: Integer) ifFalse: [
            ^InvalidArgumentError signal
        ].
        anInteger = 0 ifTrue: [
            ^ZeroDivide signal
        ].
        ^self - ((self quo: anInteger) * anInteger)
    ]

    gcd: anInteger [
        | left right remainder |
        (anInteger isKindOf: Integer) ifFalse: [
            ^InvalidArgumentError signal
        ].
        left := self abs.
        right := anInteger abs.
        [right = 0] whileFalse: [
            remainder := left rem: right.
            left := right.
            right := remainder
        ].
        ^left
    ]

    lcm: anInteger [
        (anInteger isKindOf: Integer) ifFalse: [
            ^InvalidArgumentError signal
        ].
        (self = 0 or: [anInteger = 0]) ifTrue: [
            ^0
        ].
        ^((self quo: (self gcd: anInteger)) * anInteger) abs
    ]

    numerator [
        ^self
    ]

    denominator [
        ^1
    ]

    asFraction [
        ^Fraction numerator: self denominator: 1
    ]

    truncated [
        ^self
    ]

    floor [
        ^self
    ]

    ceiling [
        ^self
    ]

    rounded [
        ^self
    ]

    isInteger [
        ^true
    ]
]
