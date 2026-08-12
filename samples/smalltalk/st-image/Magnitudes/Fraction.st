Fraction := Number [

    | numerator denominator |

    "Fraction instances are immutable through the image protocol after their
     single initialization.
     Every published instance has a nonzero numerator, a denominator greater
     than one, and relatively-prime Integer components. The public constructor
     returns the canonical Integer instead when either component would make a
     Fraction representation redundant."

    class new [
        ^self shouldNotImplement
    ]

    class new: aSize [
        ^self shouldNotImplement
    ]

    class basicNew [
        ^self shouldNotImplement
    ]

    class basicNew: aSize [
        ^self shouldNotImplement
    ]

    class numerator: aNumerator denominator: aDenominator [
        | normalizedNumerator normalizedDenominator divisor |
        (aNumerator isKindOf: Integer) ifFalse: [
            ^InvalidArgumentError signal
        ].
        (aDenominator isKindOf: Integer) ifFalse: [
            ^InvalidArgumentError signal
        ].
        aDenominator = 0 ifTrue: [
            ^ZeroDivide signal
        ].
        aNumerator = 0 ifTrue: [
            ^0
        ].

        normalizedNumerator := aNumerator.
        normalizedDenominator := aDenominator.
        normalizedDenominator < 0 ifTrue: [
            normalizedNumerator := normalizedNumerator negated.
            normalizedDenominator := normalizedDenominator negated
        ].
        divisor := normalizedNumerator gcd: normalizedDenominator.
        normalizedNumerator := normalizedNumerator quo: divisor.
        normalizedDenominator := normalizedDenominator quo: divisor.
        normalizedDenominator = 1 ifTrue: [
            ^normalizedNumerator
        ].

        ^super new
            initializeNumerator: normalizedNumerator
            denominator: normalizedDenominator
    ]

    initializeNumerator: anInteger denominator: aPositiveInteger [
        "This second validation boundary prevents a later message from
         mutating a canonical Fraction and also protects subclass allocation."
        (numerator notNil or: [denominator notNil]) ifTrue: [
            ^InvalidArgumentError signal
        ].
        (anInteger isKindOf: Integer) ifFalse: [
            ^InvalidArgumentError signal
        ].
        (aPositiveInteger isKindOf: Integer) ifFalse: [
            ^InvalidArgumentError signal
        ].
        (anInteger = 0 or: [aPositiveInteger <= 1]) ifTrue: [
            ^InvalidArgumentError signal
        ].
        (anInteger gcd: aPositiveInteger) = 1 ifFalse: [
            ^InvalidArgumentError signal
        ].
        numerator := anInteger.
        denominator := aPositiveInteger.
        ^self
    ]

    instVarAt: anIndex put: aValue [
        ^self shouldNotImplement
    ]

    numerator [
        ^numerator
    ]

    denominator [
        ^denominator
    ]

    = aNumber [
        (aNumber isKindOf: Integer) ifTrue: [
            ^self equalsInteger: aNumber
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            ^numerator = aNumber numerator
                and: [denominator = aNumber denominator]
        ].
        ^false
    ]

    < aNumber [
        | commonDenominator |
        (aNumber isKindOf: Integer) ifTrue: [
            ^numerator < (aNumber * denominator)
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            commonDenominator := denominator gcd: aNumber denominator.
            ^(numerator * (aNumber denominator quo: commonDenominator))
                < (aNumber numerator
                    * (denominator quo: commonDenominator))
        ].
        (aNumber isKindOf: Float) ifTrue: [
            ^self asFloat < aNumber
        ].
        ^InvalidArgumentError signal
    ]

    + aNumber [
        | commonDenominator leftScale rightScale |
        (aNumber isKindOf: Integer) ifTrue: [
            ^self addInteger: aNumber
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            commonDenominator := denominator gcd: aNumber denominator.
            leftScale := aNumber denominator quo: commonDenominator.
            rightScale := denominator quo: commonDenominator.
            ^Fraction
                numerator: (numerator * leftScale)
                    + (aNumber numerator * rightScale)
                denominator: denominator * leftScale
        ].
        (aNumber isKindOf: Float) ifTrue: [
            ^self asFloat + aNumber
        ].
        ^InvalidArgumentError signal
    ]

    - aNumber [
        | commonDenominator leftScale rightScale |
        (aNumber isKindOf: Integer) ifTrue: [
            ^Fraction
                numerator: numerator - (aNumber * denominator)
                denominator: denominator
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            commonDenominator := denominator gcd: aNumber denominator.
            leftScale := aNumber denominator quo: commonDenominator.
            rightScale := denominator quo: commonDenominator.
            ^Fraction
                numerator: (numerator * leftScale)
                    - (aNumber numerator * rightScale)
                denominator: denominator * leftScale
        ].
        (aNumber isKindOf: Float) ifTrue: [
            ^self asFloat - aNumber
        ].
        ^InvalidArgumentError signal
    ]

    * aNumber [
        | leftDivisor rightDivisor |
        (aNumber isKindOf: Integer) ifTrue: [
            ^self multiplyInteger: aNumber
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            leftDivisor := numerator gcd: aNumber denominator.
            rightDivisor := aNumber numerator gcd: denominator.
            ^Fraction
                numerator: (numerator quo: leftDivisor)
                    * (aNumber numerator quo: rightDivisor)
                denominator: (denominator quo: rightDivisor)
                    * (aNumber denominator quo: leftDivisor)
        ].
        (aNumber isKindOf: Float) ifTrue: [
            ^self asFloat * aNumber
        ].
        ^InvalidArgumentError signal
    ]

    / aNumber [
        | numeratorDivisor denominatorDivisor |
        (aNumber isKindOf: Integer) ifTrue: [
            aNumber = 0 ifTrue: [
                ^ZeroDivide signal
            ].
            numeratorDivisor := numerator gcd: aNumber.
            ^Fraction
                numerator: numerator quo: numeratorDivisor
                denominator: denominator
                    * (aNumber quo: numeratorDivisor)
        ].
        (aNumber isKindOf: Fraction) ifTrue: [
            aNumber numerator = 0 ifTrue: [
                ^ZeroDivide signal
            ].
            numeratorDivisor := numerator gcd: aNumber numerator.
            denominatorDivisor := aNumber denominator gcd: denominator.
            ^Fraction
                numerator: (numerator quo: numeratorDivisor)
                    * (aNumber denominator quo: denominatorDivisor)
                denominator: (denominator quo: denominatorDivisor)
                    * (aNumber numerator quo: numeratorDivisor)
        ].
        (aNumber isKindOf: Float) ifTrue: [
            ^self asFloat / aNumber
        ].
        ^InvalidArgumentError signal
    ]

    // aNumber [
        ^(self / aNumber) floor
    ]

    \\ aNumber [
        ^self - ((self // aNumber) * aNumber)
    ]

    addInteger: anInteger [
        ^Fraction
            numerator: numerator + (anInteger * denominator)
            denominator: denominator
    ]

    subtractFromInteger: anInteger [
        ^Fraction
            numerator: (anInteger * denominator) - numerator
            denominator: denominator
    ]

    multiplyInteger: anInteger [
        | divisor |
        divisor := anInteger gcd: denominator.
        ^Fraction
            numerator: numerator * (anInteger quo: divisor)
            denominator: denominator quo: divisor
    ]

    divideIntegerInto: anInteger [
        | divisor |
        numerator = 0 ifTrue: [
            ^ZeroDivide signal
        ].
        divisor := anInteger gcd: numerator.
        ^Fraction
            numerator: (anInteger quo: divisor) * denominator
            denominator: numerator quo: divisor
    ]

    equalsInteger: anInteger [
        ^numerator = (anInteger * denominator)
    ]

    greaterThanInteger: anInteger [
        ^numerator > (anInteger * denominator)
    ]

    negated [
        ^Fraction numerator: numerator negated denominator: denominator
    ]

    reciprocal [
        numerator = 0 ifTrue: [
            ^ZeroDivide signal
        ].
        ^Fraction numerator: denominator denominator: numerator
    ]

    quo: aNumber [
        ^(self / aNumber) truncated
    ]

    rem: aNumber [
        ^self - ((self quo: aNumber) * aNumber)
    ]

    truncated [
        ^numerator quo: denominator
    ]

    floor [
        ^numerator // denominator
    ]

    ceiling [
        ^(numerator negated // denominator) negated
    ]

    rounded [
        | quotient remainder |
        quotient := self truncated.
        remainder := numerator rem: denominator.
        remainder abs * 2 < denominator ifTrue: [
            ^quotient
        ].
        numerator < 0 ifTrue: [
            ^quotient - 1
        ].
        ^quotient + 1
    ]

    asFloat [
        ^numerator asFloat / denominator asFloat
    ]

    asFraction [
        ^self
    ]

    hash [
        ^numerator hash bitXor: denominator hash
    ]

    isFraction [
        ^true
    ]
]
