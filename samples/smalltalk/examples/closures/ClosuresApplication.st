ClosuresApplication := Object [

    makeAdder: offset [
        ^[:value | value + offset]
    ]

    makeSelf [
        ^[self]
    ]

    nonLocalAnswer [
        ^[ ^42 ] value
    ]

    run [
        | addTwo |
        addTwo := self makeAdder: 2.
        (self makeSelf) value.
        self nonLocalAnswer.
        ^addTwo value: 40
    ]
]
