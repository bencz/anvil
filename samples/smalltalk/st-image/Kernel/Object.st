Object := nil [

    yourself [
        ^self
    ]

    = anObject [
        <primitive: IdentityPrimitive>
    ]

    == anObject [
        <primitive: IdentityPrimitive>
    ]

    ~= anObject [
        ^(self = anObject) not
    ]

    ~~ anObject [
        ^(self == anObject) not
    ]

    class [
        <primitive: ClassPrimitive>
    ]

    hash [
        <primitive: HashPrimitive>
    ]

    identityHash [
        <primitive: HashPrimitive>
    ]

    -> anObject [
        ^Association key: self value: anObject
    ]

    isKindOf: aClass [
        | candidate |
        candidate := self class.
        [candidate notNil] whileTrue: [
            candidate == aClass ifTrue: [^true].
            candidate := candidate superClass
        ].
        ^false
    ]

    isMemberOf: aClass [
        ^self class == aClass
    ]

    respondsTo: aSelector [
        ^self class canUnderstand: aSelector
    ]

    isString [
        ^false
    ]

    isSymbol [
        ^false
    ]

    size [
        <primitive: SizePrimitive>
    ]

    basicSize [
        <primitive: SizePrimitive>
    ]

    at: anIndex [
        <primitive: AtPrimitive>
        ^OutOfRangeError signal
    ]

    at: anIndex put: aValue [
        <primitive: AtPutPrimitive>
        ^OutOfRangeError signal
    ]

    instVarAt: anIndex [
        <primitive: InstVarAtPrimitive>
        ^OutOfRangeError signal
    ]

    instVarAt: anIndex put: aValue [
        <primitive: InstVarAtPutPrimitive>
        ^OutOfRangeError signal
    ]

    isNil [
        ^false
    ]

    notNil [
        ^true
    ]

    ifNil: nilBlock [
        ^self
    ]

    ifNotNil: notNilBlock [
        ^notNilBlock value: self
    ]

    ifNil: nilBlock ifNotNil: notNilBlock [
        ^notNilBlock value: self
    ]

    ifNotNil: notNilBlock ifNil: nilBlock [
        ^notNilBlock value: self
    ]

    subclassResponsibility [
        ^SubClassResponsibility signal
    ]

    shouldNotImplement [
        ^ShouldNotImplement signal
    ]

    doesNotUnderstand: aMessage [
        ^MessageNotUnderstood new
            receiver: self;
            message: aMessage;
            signal
    ]
]
