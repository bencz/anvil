CompiledMethod := Object [

    <shape: fixedPointers>
    <abstract: true>

    | selector methodClass numArgs |

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

    selector [
        ^selector
    ]

    methodClass [
        ^methodClass
    ]

    numArgs [
        ^numArgs
    ]
]
