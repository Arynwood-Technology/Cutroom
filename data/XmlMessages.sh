# SPDX-FileCopyrightText: Pino Toscano  <pino@kde.org>
# SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

function get_files
{
    echo com.arynwood.Cutroom.xml
}

function po_for_file
{
    case "$1" in
       com.arynwood.Cutroom.xml)
           echo kdenlive_xml_mimetypes.po
       ;;
    esac
}

function tags_for_file
{
    case "$1" in
       com.arynwood.Cutroom.xml)
           echo comment
       ;;
    esac
}
