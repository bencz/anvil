Block := Object [

    <abstract: true>
    <shape: closure>

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

    value [
        <primitive: BlockValuePrimitive>
        ^Error signal
    ]

    value: argument [
        <primitive: BlockValuePrimitive1>
        ^Error signal
    ]

    value: first value: second [
        <primitive: BlockValuePrimitive2>
        ^Error signal
    ]

    value: first value: second value: third [
        <primitive: BlockValuePrimitive3>
        ^Error signal
    ]

    valueWithArguments: arguments [
        <primitive: BlockValueArgsPrimitive>
        ^Error signal
    ]

    whileTrue: aBlock [
        <primitive: BlockWhileTruePrimitive>
        ^Error signal
    ]

    whileFalse: aBlock [
        ^[self value not] whileTrue: aBlock
    ]

    ensure: aBlock [
        <primitive: BlockUnwindPrimitive>
        ^InvalidArgumentError signal:
            'ensure: requires two valid zero-argument blocks'
    ]

    on: anExceptionClass do: aBlock [
        <primitive: BlockOnExceptionPrimitive>
        ^InvalidArgumentError signal:
            'on:do: requires an exception class and a one-argument handler block'
    ]
]
