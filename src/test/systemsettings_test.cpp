#include "preferences/systemsettings.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QStringList>

namespace {

class ScopedUserEnvironment {
  public:
    explicit ScopedUserEnvironment(const QByteArray& user)
            : m_wasSet(qEnvironmentVariableIsSet("USER")),
              m_previous(qgetenv("USER")) {
        qputenv("USER", user);
    }

    ~ScopedUserEnvironment() {
        if (m_wasSet) {
            qputenv("USER", m_previous);
        } else {
            qunsetenv("USER");
        }
    }

  private:
    bool m_wasSet;
    QByteArray m_previous;
};

TEST(SystemSettingsTest, RemovableRootsIncludeCurrentUserMountDirectories) {
    const ScopedUserEnvironment user("bitedj-test-user");

    const QStringList roots = SystemSettings::removableRoots();

    EXPECT_TRUE(roots.contains(QStringLiteral("/media/bitedj-test-user")));
    EXPECT_TRUE(roots.contains(QStringLiteral("/run/media/bitedj-test-user")));
}

TEST(SystemSettingsTest, RemovableRootsDoNotDuplicateBasePathsWithoutUser) {
    const ScopedUserEnvironment user("");

    const QStringList roots = SystemSettings::removableRoots();

    EXPECT_EQ(1, roots.count(QStringLiteral("/media")));
    EXPECT_EQ(1, roots.count(QStringLiteral("/run/media")));
    EXPECT_FALSE(roots.contains(QStringLiteral("/media/")));
    EXPECT_FALSE(roots.contains(QStringLiteral("/run/media/")));
}

QList<SystemSettings::UsbMount> lexarMounted() {
    return {SystemSettings::UsbMount{
            QStringLiteral("/dev/sda1"),
            QStringLiteral("/media/bitedj-test-user/Lexar")}};
}

TEST(SystemSettingsTest, AbsentDriveNamesTheStickThatIsNotMounted) {
    const ScopedUserEnvironment user("bitedj-test-user");

    // The longest matching root is the per-user one, so the drive is the
    // directory below it and not the user's name.
    EXPECT_EQ(QStringLiteral("ESD-USB"),
            SystemSettings::absentDrive(
                    QStringLiteral("/media/bitedj-test-user/ESD-USB/Contents/"
                                   "1991/Pleasure/Pleasure - 1991.mp3"),
                    lexarMounted()));
    EXPECT_EQ(QStringLiteral("Stick"),
            SystemSettings::absentDrive(
                    QStringLiteral("/mnt/Stick/track.mp3"), lexarMounted()));
}

TEST(SystemSettingsTest, AbsentDriveIsEmptyForAMountedStick) {
    const ScopedUserEnvironment user("bitedj-test-user");

    EXPECT_TRUE(SystemSettings::absentDrive(
            QStringLiteral("/media/bitedj-test-user/Lexar/Music/track.mp3"),
            lexarMounted())
                    .isEmpty());
}

TEST(SystemSettingsTest, AbsentDriveMatchesWholeMountPointNames) {
    const ScopedUserEnvironment user("bitedj-test-user");

    // "Lexar" is mounted, "Lexar2" is not: a prefix of the name is not a match.
    EXPECT_EQ(QStringLiteral("Lexar2"),
            SystemSettings::absentDrive(
                    QStringLiteral("/media/bitedj-test-user/Lexar2/track.mp3"),
                    lexarMounted()));
}

TEST(SystemSettingsTest, AbsentDriveIsEmptyOffRemovableMedia) {
    const ScopedUserEnvironment user("bitedj-test-user");

    EXPECT_TRUE(SystemSettings::absentDrive(
            QStringLiteral("/home/bitedj-test-user/Music/track.mp3"),
            lexarMounted())
                    .isEmpty());
    // A file directly in a root directory is not on any drive.
    EXPECT_TRUE(SystemSettings::absentDrive(
            QStringLiteral("/media/bitedj-test-user/track.mp3"),
            lexarMounted())
                    .isEmpty());
}

} // namespace
