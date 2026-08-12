Behavior := Object [

    <abstract: true>

    | superClass subClasses methodDictionary instanceVariables |

    new [
        <primitive: BehaviorNewPrimitive>
    ]

    new: aSize [
        <primitive: BehaviorNewSizePrimitive>
        ^InvalidArgumentError signal
    ]

    basicNew [
        <primitive: BehaviorNewPrimitive>
    ]

    basicNew: aSize [
        <primitive: BehaviorNewSizePrimitive>
        ^InvalidArgumentError signal
    ]

    superClass [
        ^superClass
    ]

    subClasses [
        ^subClasses
    ]

    methodDictionary [
        ^methodDictionary
    ]

    instanceVariables [
        ^instanceVariables
    ]

    lookupSelector: aSelector [
        <primitive: BehaviorLookupSelectorPrimitive>
        ^InvalidArgumentError signal
    ]

    canUnderstand: aSelector [
        ^(self lookupSelector: aSelector) notNil
    ]
]
