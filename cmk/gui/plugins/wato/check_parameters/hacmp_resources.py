#!/usr/bin/env python
# -*- coding: utf-8 -*-
# Copyright (C) 2019 tribe29 GmbH - License: GNU General Public License v2
# This file is part of Checkmk (https://checkmk.com). It is subject to the terms and
# conditions defined in the file COPYING, which is part of this source code package.

from cmk.gui.i18n import _
from cmk.gui.valuespec import (
    Dictionary,
    DropdownChoice,
    TextAscii,
)

from cmk.gui.plugins.wato import (
    CheckParameterRulespecWithItem,
    rulespec_registry,
    RulespecGroupCheckParametersApplications,
)


def _parameter_valuespec_hacmp_resources():
    return Dictionary(
        elements=[
            ("expect_online_on",
             DropdownChoice(
                 title=_(u"Expect resource to be online on"),
                 choices=[
                     ("first", _(u"the first node")),
                     ("any", _(u"any node")),
                 ],
             )),
        ],
        optional_keys=[],
    )


rulespec_registry.register(
    CheckParameterRulespecWithItem(
        check_group_name="hacmp_resources",
        group=RulespecGroupCheckParametersApplications,
        item_spec=lambda: TextAscii(title=_("Resource Group")),
        parameter_valuespec=_parameter_valuespec_hacmp_resources,
        title=lambda: _("AIX HACMP Resource Groups"),
    ))
