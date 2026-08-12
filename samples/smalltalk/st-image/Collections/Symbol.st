Symbol := String [

    <abstract: true>

    class new [
        ^self shouldNotImplement
    ]

    class new: size [
        ^self shouldNotImplement
    ]

    class basicNew [
        ^self shouldNotImplement
    ]

    class basicNew: size [
        ^self shouldNotImplement
    ]

    asSymbol [
        ^self
    ]

    isSymbol [
        ^true
    ]

    at: anIndex put: aValue [
        ^self shouldNotImplement
    ]
]
